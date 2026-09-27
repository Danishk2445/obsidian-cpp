#pragma once

// A small in-memory vault for the Bases engine tests (dates are interpreted in UTC).

#include "core/bases/base_query.h"

#include <QString>

#include <gtest/gtest.h>

#include <ctime>
#include <cstdlib>
#include <ostream>

inline void PrintTo(const QString& s, std::ostream* os) { *os << '"' << s.toStdString() << '"'; }

namespace bases_test {

// Local time = UTC, so date arithmetic in the expectations is deterministic.
inline const bool kUtc = [] {
    setenv("TZ", "UTC", 1);
    tzset();
    return true;
}();

inline void addBooks(oc::bases::MemoryDataSource& ds)
{
    ds.addFile(QStringLiteral("Books/Dune.md"), QStringLiteral(R"(---
author: Frank Herbert
pages: 412
rating: 4.5
status: read
price: 9.99
tags:
  - book
  - scifi/classic
published: 1965-08-01
series: "[[Dune Chronicles]]"
site: https://example.com/dune
started: 2024-01-15T10:30:00
---
# Dune
A classic by [[Frank Herbert]], compare [[Hyperion]]. #desert
)"),
               1700000000000, 1710000000000);
    ds.addFile(QStringLiteral("Books/Hyperion.md"), QStringLiteral(R"(---
author: Dan Simmons
pages: 482
rating: 4
status: reading
price: 12.5
tags: [book, scifi]
published: 1989-05-26
---
Shrike. [[Dune]]
)"),
               1700000001000, 1710000001000);
    ds.addFile(QStringLiteral("Books/Neuromancer.md"), QStringLiteral(R"(---
author: William Gibson
pages: 271
rating: 3.5
status: read
tags: book
published: 1984-07-01
---
Cyberspace. ![[cover.png]]
)"),
               1700000002000, 1710000002000);
    ds.addFile(QStringLiteral("Books/Draft.md"), QStringLiteral("No properties here. #draft\n"), 1700000003000, 1710000003000);
    ds.addFile(QStringLiteral("Authors/Frank Herbert.md"), QStringLiteral("---\ntype: author\nborn: 1920\n---\nWrote [[Dune]].\n"),
               1700000004000, 1710000004000);
    ds.addFile(QStringLiteral("cover.png"), QString(), 1700000005000, 1710000005000);
    ds.addFile(QStringLiteral("Books.base"), QString(), 1700000006000, 1710000006000);
}

struct Env {
    oc::bases::MemoryDataSource ds;
    oc::bases::BaseFile base;
    std::shared_ptr<oc::bases::QueryContext> ctx;

    Env()
    {
        (void)kUtc;
        addBooks(ds);
        base = *oc::bases::BaseFile::fromString(QString());
        reset();
    }
    void reset(const QString& thisPath = QStringLiteral("Books.base"))
    {
        ctx = oc::bases::QueryContext::create(&ds, base, base.view(QString()), thisPath);
    }
    oc::bases::EntryPtr entry(const QString& path = QStringLiteral("Books/Dune.md")) { return std::make_shared<oc::bases::Entry>(ctx, path); }
    oc::bases::Value eval(const QString& expr, const QString& path = QStringLiteral("Books/Dune.md"))
    {
        oc::bases::Formula f(expr);
        return f.value(*entry(path));
    }
};

} // namespace bases_test
