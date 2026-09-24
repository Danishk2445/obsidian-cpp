#pragma once

// Synthetic vault graphs for the graph widget tests, screenshots and perf gates: notes with
// readable names, Zipf-distributed links (a few hubs, a long tail), tags, unresolved links,
// attachments and some orphans. Deterministic for a given seed.

#include "graph/graph_data.h"

#include <QStringList>

#include <cmath>
#include <memory>
#include <random>

namespace oc::test {

inline QString syntheticNoteName(int i, std::mt19937& rng)
{
    static const QStringList first = {"Project", "Meeting", "Idea",     "Reading",  "Journal", "Research", "Draft",  "Recipe",
                                      "Travel",  "Book",    "Workout",  "Garden",   "Budget",  "Lecture",  "Review", "Plan",
                                      "Music",   "Film",    "Language", "Habit",    "Weekly",  "Quarterly", "Design", "Bug"};
    static const QStringList second = {"notes",   "log",     "summary", "ideas",  "outline", "tracker", "checklist", "archive",
                                       "roadmap", "sketch",  "review",  "digest", "journal", "index",   "overview",  "questions"};
    const QString a = first[int(rng() % unsigned(first.size()))];
    const QString b = second[int(rng() % unsigned(second.size()))];
    return QStringLiteral("%1 %2 %3").arg(a, b).arg(i);
}

struct SyntheticGraphOptions {
    int notes = 300;
    double linksPerNote = 2.2;
    int tags = 12;
    int unresolved = 24;
    int attachments = 12;
    double orphanRatio = 0.08;
    unsigned seed = 7;
};

inline std::shared_ptr<graph::GraphData> makeSyntheticGraph(const SyntheticGraphOptions& o = {})
{
    using graph::NodeKind;
    std::mt19937 rng(o.seed);
    auto g = std::make_shared<graph::GraphData>();
    QStringList folders = {"", "Projects/", "Areas/", "Resources/", "Journal/"};
    std::vector<int> notes;
    for (int i = 0; i < o.notes; ++i) {
        const QString path = folders[int(rng() % unsigned(folders.size()))] + syntheticNoteName(i, rng) + ".md";
        notes.push_back(g->addNode(path, NodeKind::File, {}, 1'600'000'000'000LL + qint64(i) * 3'600'000));
    }
    std::vector<int> attachments;
    for (int i = 0; i < o.attachments; ++i)
        attachments.push_back(g->addNode(QStringLiteral("Attachments/image %1.png").arg(i), NodeKind::Attachment, {}, 1'600'000'000'000LL + i));
    // Zipf target choice: P(rank r) ∝ r^-1.1.
    std::vector<double> cdf(size_t(o.notes));
    double acc = 0;
    for (int r = 0; r < o.notes; ++r) {
        acc += std::pow(double(r + 1), -1.1);
        cdf[size_t(r)] = acc;
    }
    auto zipf = [&] {
        const double u = std::uniform_real_distribution<double>(0, acc)(rng);
        return int(std::lower_bound(cdf.begin(), cdf.end(), u) - cdf.begin());
    };
    std::vector<QString> unresolved;
    for (int i = 0; i < o.unresolved; ++i)
        unresolved.push_back(QStringLiteral("Missing %1").arg(syntheticNoteName(1000 + i, rng)));
    std::vector<QString> tags;
    for (int i = 0; i < o.tags; ++i)
        tags.push_back(QStringLiteral("#%1").arg(QStringList{"work", "personal", "idea", "todo", "reading", "health", "finance",
                                                             "travel", "learning", "project/alpha", "project/beta", "archive", "draft",
                                                             "people", "meeting"}[i % 15]));
    std::poisson_distribution<int> linkCount(o.linksPerNote);
    for (int i = 0; i < o.notes; ++i) {
        if (std::uniform_real_distribution<double>(0, 1)(rng) < o.orphanRatio)
            continue;
        const int k = std::max(1, linkCount(rng));
        for (int j = 0; j < k; ++j) {
            const int t = notes[size_t(zipf())];
            if (t != notes[size_t(i)])
                g->addEdge(notes[size_t(i)], t);
        }
        if (!unresolved.empty() && rng() % 9 == 0) {
            const int u = g->addNode(unresolved[rng() % unresolved.size()], NodeKind::Unresolved);
            g->addEdge(notes[size_t(i)], u);
        }
        if (!attachments.empty() && rng() % 11 == 0)
            g->addEdge(notes[size_t(i)], attachments[rng() % attachments.size()]);
        if (!tags.empty() && rng() % 3 == 0) {
            const int tg = g->addNode(tags[rng() % tags.size()], NodeKind::Tag);
            g->addEdge(notes[size_t(i)], tg);
        }
    }
    g->finalize();
    return g;
}

} // namespace oc::test
