#include "Ctracker.h"

///
/// \brief CTracker::CTracker
/// Tracker. Manage tracks. Create, remove, update.
/// \param settings
///
CTracker::CTracker(const TrackerSettings& settings)
    :
      m_settings(settings),
      m_nextTrackID(0)
{
    m_SPCalculator.reset();
    SPSettings spSettings = { settings.m_distThres, 12 };
    switch (m_settings.m_matchType)
    {
    case tracking::MatchHungrian:
        m_SPCalculator = std::make_unique<SPHungrian>(spSettings);
        break;
    case tracking::MatchBipart:
        m_SPCalculator = std::make_unique<SPBipart>(spSettings);
        break;
    }
    assert(m_SPCalculator);

	for (const auto& embParam : settings.m_embeddings)
	{
		std::shared_ptr<EmbeddingsCalculator> embCalc = std::make_shared<EmbeddingsCalculator>();
		if (!embCalc->Initialize(embParam.m_embeddingCfgName, embParam.m_embeddingWeightsName, embParam.m_inputLayer))
		{
			std::cerr << "EmbeddingsCalculator initialization error: " << embParam.m_embeddingCfgName << ", " << embParam.m_embeddingWeightsName << std::endl;
		}
		else
		{
			for (auto objType : embParam.m_objectTypes)
			{
				m_embCalculators.try_emplace((objtype_t)objType, embCalc);
			}
		}
	}
}

///
/// \brief CTracker::~CTracker
///
CTracker::~CTracker(void)
{
}

///
/// \brief CTracker::Update
/// \param regions
/// \param currFrame
/// \param fps
///
void CTracker::Update(const regions_t& regions,
                      cv::UMat currFrame,
                      float fps)
{
    UpdateTrackingState(regions, currFrame, fps);

    currFrame.copyTo(m_prevFrame);
}

///
/// \brief CTracker::UpdateTrackingState
/// \param regions
/// \param currFrame
/// \param fps
///
void CTracker::UpdateTrackingState(const regions_t& regions,
                                   cv::UMat currFrame,
                                   float fps)
{
    const size_t N = m_tracks.size();	// Tracking objects
    const size_t M = regions.size();	// Detections or regions

    assignments_t assignment(N, -1); // Assignments regions -> tracks

    std::vector<RegionEmbedding> regionEmbeddings;
    CalcEmbeddins(regionEmbeddings, regions, currFrame);

    if (!m_tracks.empty())
    {
        // Distance matrix between all tracks to all regions
        distMatrix_t costMatrix(N * M);
        const track_t maxPossibleCost = static_cast<track_t>(currFrame.cols * currFrame.rows);
        track_t maxCost = 0;
        CreateDistaceMatrix(regions, regionEmbeddings, costMatrix, maxPossibleCost, maxCost);

        // Solving assignment problem (shortest paths)
        m_SPCalculator->Solve(costMatrix, N, M, assignment, maxCost);

        // clean assignment from pairs with large distance
        for (size_t i = 0; i < assignment.size(); i++)
        {
            if (assignment[i] != -1)
            {
				if (costMatrix[i + assignment[i] * N] > m_settings.m_distThres)
                {
                    assignment[i] = -1;
                    m_tracks[i]->SkippedFrames()++;
                }
            }
            else
            {
                // If track have no assigned detect, then increment skipped frames counter.
                m_tracks[i]->SkippedFrames()++;
            }
        }

        // If track didn't get detects long time, remove it.
        for (size_t i = 0; i < m_tracks.size();)
        {
            if (m_tracks[i]->SkippedFrames() > m_settings.m_maximumAllowedSkippedFrames ||
				m_tracks[i]->IsOutOfTheFrame() ||
                    m_tracks[i]->IsStaticTimeout(cvRound(fps * (m_settings.m_maxStaticTime - m_settings.m_minStaticTime))))
            {
                m_tracks.erase(m_tracks.begin() + i);
                assignment.erase(assignment.begin() + i);
            }
			else
			{
				++i;
			}
        }
    }

    // Search for unassigned detects and start new tracks for them.
    for (size_t i = 0; i < regions.size(); ++i)
    {
        if (find(assignment.begin(), assignment.end(), i) == assignment.end())
        {
            if (regionEmbeddings.empty())
                m_tracks.push_back(std::make_unique<CTrack>(regions[i],
                                                            m_settings.m_kalmanType,
                                                            m_settings.m_dt,
                                                            m_settings.m_accelNoiseMag,
                                                            m_settings.m_useAcceleration,
                                                            m_nextTrackID++,
                                                            m_settings.m_filterGoal == tracking::FilterRect,
                                                            m_settings.m_lostTrackType));
            else
                m_tracks.push_back(std::make_unique<CTrack>(regions[i],
                                                            regionEmbeddings[i],
                                                            m_settings.m_kalmanType,
                                                            m_settings.m_dt,
                                                            m_settings.m_accelNoiseMag,
                                                            m_settings.m_useAcceleration,
                                                            m_nextTrackID++,
                                                            m_settings.m_filterGoal == tracking::FilterRect,
                                                            m_settings.m_lostTrackType));
        }
    }

    // Update Kalman Filters state
    const ptrdiff_t stop_i = static_cast<ptrdiff_t>(assignment.size());
#pragma omp parallel for
    for (ptrdiff_t i = 0; i < stop_i; ++i)
    {
        // If track updated less than one time, than filter state is not correct.
        if (assignment[i] != -1) // If we have assigned detect, then update using its coordinates,
        {
            m_tracks[i]->SkippedFrames() = 0;
            std::cout << "Update track " << i << " for " << assignment[i] << " region, regionEmbeddings.size = " << regionEmbeddings.size() << std::endl;
            if (regionEmbeddings.empty())
                m_tracks[i]->Update(regions[assignment[i]],
                        true, m_settings.m_maxTraceLength,
                        m_prevFrame, currFrame,
                        m_settings.m_useAbandonedDetection ? cvRound(m_settings.m_minStaticTime * fps) : 0, m_settings.m_maxSpeedForStatic);
            else
                m_tracks[i]->Update(regions[assignment[i]], regionEmbeddings[assignment[i]],
                        true, m_settings.m_maxTraceLength,
                        m_prevFrame, currFrame,
                        m_settings.m_useAbandonedDetection ? cvRound(m_settings.m_minStaticTime * fps) : 0, m_settings.m_maxSpeedForStatic);
        }
        else				     // if not continue using predictions
        {
            m_tracks[i]->Update(CRegion(), false, m_settings.m_maxTraceLength, m_prevFrame, currFrame, 0, m_settings.m_maxSpeedForStatic);
        }
    }
}

///
/// \brief CTracker::CreateDistaceMatrix
/// \param regions
/// \param costMatrix
/// \param maxPossibleCost
/// \param maxCost
///
void CTracker::CreateDistaceMatrix(const regions_t& regions,
                                   const std::vector<RegionEmbedding>& regionEmbeddings,
                                   distMatrix_t& costMatrix,
                                   track_t maxPossibleCost,
                                   track_t& maxCost)
{
    const size_t N = m_tracks.size();	// Tracking objects
    maxCost = 0;

	for (size_t i = 0; i < N; ++i)
	{
		const auto& track = m_tracks[i];

		// Calc predicted area for track
		cv::Size_<track_t> minRadius;
		if (m_settings.m_minAreaRadiusPix < 0)
		{
			minRadius.width = m_settings.m_minAreaRadiusK * track->LastRegion().m_rrect.size.width;
			minRadius.height = m_settings.m_minAreaRadiusK * track->LastRegion().m_rrect.size.height;
		}
		else
		{
			minRadius.width = m_settings.m_minAreaRadiusPix;
			minRadius.height = m_settings.m_minAreaRadiusPix;
		}
		cv::RotatedRect predictedArea = track->CalcPredictionEllipse(minRadius);

		// Calc distance between track and regions
		for (size_t j = 0; j < regions.size(); ++j)
		{
			const auto& reg = regions[j];

			auto dist = maxPossibleCost;
			if (m_settings.CheckType(m_tracks[i]->LastRegion().m_type, reg.m_type))
			{
				dist = 0;
				size_t ind = 0;
				// Euclidean distance between centers
				if (m_settings.m_distType[ind] > 0.0f && ind == tracking::DistCenters)
				{
#if 1
                    track_t ellipseDist = track->IsInsideArea(reg.m_rrect.center, predictedArea);
                    if (ellipseDist > 1)
                        dist += m_settings.m_distType[ind];
                    else
                        dist += ellipseDist * m_settings.m_distType[ind];
#else
					dist += m_settings.m_distType[ind] * track->CalcDistCenter(reg);
#endif
				}
				++ind;

				// Euclidean distance between bounding rectangles
				if (m_settings.m_distType[ind] > 0.0f && ind == tracking::DistRects)
				{
#if 1
                    track_t ellipseDist = track->IsInsideArea(reg.m_rrect.center, predictedArea);
					if (ellipseDist < 1)
					{
						track_t dw = track->WidthDist(reg);
						track_t dh = track->HeightDist(reg);
						dist += m_settings.m_distType[ind] * (1 - (1 - ellipseDist) * (dw + dh) * 0.5f);
					}
					else
					{
						dist += m_settings.m_distType[ind];
					}
					//std::cout << "dist = " << dist << ", ed = " << ellipseDist << ", dw = " << dw << ", dh = " << dh << std::endl;
#else
					dist += m_settings.m_distType[ind] * track->CalcDistRect(reg);
#endif
				}
				++ind;

				// Intersection over Union, IoU
				if (m_settings.m_distType[ind] > 0.0f && ind == tracking::DistJaccard)
					dist += m_settings.m_distType[ind] * track->CalcDistJaccard(reg);
				++ind;

				// Bhatacharia distance between histograms
				if (m_settings.m_distType[ind] > 0.0f && ind == tracking::DistHist)
                {
                    dist += m_settings.m_distType[ind] * track->CalcDistHist(regionEmbeddings[j]);
                }
				++ind;

				// Cosine distance between embeddings
                if (m_settings.m_distType[ind] > 0.0f && ind == tracking::DistFeatureCos)
                {
                    if (reg.m_type == track->LastRegion().m_type)
                    {
                        std::cout << "CalcCosine: " << TypeConverter::Type2Str(track->LastRegion().m_type) << ", reg = " << reg.m_brect << ", track = " << track->LastRegion().m_brect << std::endl;
                        dist += m_settings.m_distType[ind] * track->CalcCosine(regionEmbeddings[j]);
                    }
                }
				++ind;
				assert(ind == tracking::DistsCount);
			}

			costMatrix[i + j * N] = dist;
			if (dist > maxCost)
				maxCost = dist;
		}
	}
}

///
/// \brief CTracker::CalcEmbeddins
/// \param regionEmbeddings
/// \param regions
/// \param currFrame
///
void CTracker::CalcEmbeddins(std::vector<RegionEmbedding>& regionEmbeddings, const regions_t& regions, cv::UMat currFrame) const
{
    if (!regions.empty())
    {
        regionEmbeddings.resize(regions.size());
        // Bhatacharia distance between histograms
        if (m_settings.m_distType[tracking::DistHist] > 0.0f)
        {
            for (size_t j = 0; j < regions.size(); ++j)
            {
                    int bins = 64;
                    std::vector<int> histSize;
                    std::vector<float> ranges;
                    std::vector<int> channels;

                    for (int i = 0, stop = currFrame.channels(); i < stop; ++i)
                    {
                        histSize.push_back(bins);
                        ranges.push_back(0);
                        ranges.push_back(255);
                        channels.push_back(i);
                    }

                    std::vector<cv::UMat> regROI = { currFrame(regions[j].m_brect) };
                    cv::calcHist(regROI, channels, cv::Mat(), regionEmbeddings[j].m_hist, histSize, ranges, false);
                    cv::normalize(regionEmbeddings[j].m_hist, regionEmbeddings[j].m_hist, 0, 1, cv::NORM_MINMAX, -1, cv::Mat());
            }
        }

        // Cosine distance between embeddings
        if (m_settings.m_distType[tracking::DistFeatureCos] > 0.0f)
        {
            for (size_t j = 0; j < regions.size(); ++j)
            {
                if (regionEmbeddings[j].m_embedding.empty())
                {
                    std::cout << "Search embCalc for " << TypeConverter::Type2Str(regions[j].m_type) << ": ";
                    auto embCalc = m_embCalculators.find(regions[j].m_type);
                    if (embCalc != std::end(m_embCalculators))
                    {
                        embCalc->second->Calc(currFrame, regions[j].m_brect, regionEmbeddings[j].m_embedding);
                        regionEmbeddings[j].m_embDot = regionEmbeddings[j].m_embedding.dot(regionEmbeddings[j].m_embedding);

                        std::cout << "Founded! m_embedding = " << regionEmbeddings[j].m_embedding.size() << ", m_embDot = " << regionEmbeddings[j].m_embDot << std::endl;
                    }
                    else
                    {
                        std::cout << "Not found" << std::endl;
                    }
                }
            }
        }
    }
}
