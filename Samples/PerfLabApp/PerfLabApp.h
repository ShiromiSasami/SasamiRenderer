#pragma once

#include "IApplication.h"
#include "Object/Camera.h"
#include "PerfCaseRunner.h"
#include "PerfRegression.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <boost/signals2/connection.hpp>

namespace SasamiEngine::Sample::PerfLab
{
    // Benchmark lab: each case is a baseline/optimised pair measured N times (median/min/max,
    // cold run kept apart) and checked for equal results. The scene is only a floor and a sun.
    class PerfLabApp : public IApplication
    {
    public:
        void OnInit(ApplicationCore& app) override;
        void OnUpdate(ApplicationCore& app, float deltaTime) override;
        void OnRender(ApplicationCore& app) override;
        void OnShutdown(ApplicationCore& app) override;

    private:
        void RegisterCases(ApplicationCore& app);
        void RegisterCommands(ApplicationCore& app);
        std::string RunCaseCommand(const std::vector<std::string>& args);
        std::string StatusText() const;
        std::string ResultLine(const PerfCaseResult& result) const;
        void WriteFinishedCsv();
        std::string ComparisonText(const PerfComparison& comparison) const;
        void DrawComparisonCell(const PerfCaseResult& result) const;
        void DrawPanel();

        std::vector<boost::signals2::scoped_connection> m_inputConnections;
        Camera* m_camera = nullptr;
        PerfCaseRunner m_runner;
        std::filesystem::path m_csvDir;
        std::string m_lastCsvError;
        int m_uiRepeat = 5;
        // Per case: this session's last result against the CSV it overwrote.
        std::map<std::string, PerfComparison> m_comparisons;
        float m_regressionPct = 10.0f;
    };
}
