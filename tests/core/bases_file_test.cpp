// .base files: reading Obsidian's YAML, keeping unknown keys, and writing the model back in the
// format Obsidian writes (byte-identical when nothing changed).

#include "core/bases/base_file.h"
#include "core/bases_test_support.h"

#include <gtest/gtest.h>

using namespace oc::bases;

namespace {

// A base as Obsidian 1.13 writes it.
const char* const kObsidianBase = R"(filters:
  and:
    - file.hasTag("book")
    - status != "abandoned"
formulas:
  price_per_page: (price / pages).toFixed(2)
  age: (today() - date(published)).days
properties:
  note.author:
    displayName: Author
  formula.price_per_page:
    displayName: Price per page
views:
  - type: table
    name: All books
    order:
      - file.name
      - author
      - pages
      - formula.price_per_page
    sort:
      - property: author
        direction: ASC
    columnSize:
      file.name: 220
      note.author: 180
  - type: cards
    name: Gallery
    groupBy:
      property: status
      direction: DESC
    limit: 10
    image: note.cover
    cardSize: 180
)";

TEST(BasesFile, ObsidianOutputRoundTripsByteForByte)
{
    const QString text = QString::fromUtf8(kObsidianBase);
    QString error;
    const std::optional<BaseFile> base = BaseFile::fromString(text, &error);
    ASSERT_TRUE(base) << error.toStdString();
    EXPECT_EQ(base->toString(), text);
    ASSERT_EQ(base->views.size(), 2);
    const BaseView& table = base->views[0];
    EXPECT_EQ(table.type, QStringLiteral("table"));
    EXPECT_EQ(table.columns(), (QStringList{QStringLiteral("file.name"), QStringLiteral("note.author"), QStringLiteral("note.pages"),
                                            QStringLiteral("formula.price_per_page")}));
    ASSERT_EQ(table.sortSpecs().size(), 1);
    EXPECT_EQ(table.sortSpecs()[0].property, QStringLiteral("note.author"));
    EXPECT_EQ(table.option(QStringLiteral("columnSize")).value(QStringLiteral("note.author")).toNumber(), 180);
    const BaseView& cards = base->views[1];
    ASSERT_TRUE(cards.groupBy);
    EXPECT_EQ(cards.groupBy->property, QStringLiteral("note.status"));
    EXPECT_EQ(cards.limit, 10);
    EXPECT_EQ(cards.option(QStringLiteral("image")).toString(), QStringLiteral("note.cover"));
    EXPECT_EQ(base->displayName(QStringLiteral("note.author")), QStringLiteral("Author"));
    EXPECT_EQ(base->displayName(QStringLiteral("formula.price_per_page")), QStringLiteral("Price per page"));
    EXPECT_EQ(base->displayName(QStringLiteral("file.name")), QStringLiteral("file name"));
    EXPECT_EQ(base->displayName(QStringLiteral("note.pages")), QStringLiteral("pages"));
    ASSERT_TRUE(base->filters);
    EXPECT_EQ(base->filters->kind, Filter::Kind::And);
    EXPECT_EQ(base->filters->children.size(), 2);
    EXPECT_EQ(base->formulaNames(), (QStringList{QStringLiteral("price_per_page"), QStringLiteral("age")}));
}

TEST(BasesFile, ChangesAreWrittenLikeObsidian)
{
    BaseFile base = *BaseFile::fromString(QString::fromUtf8(kObsidianBase));
    BaseView* v = base.view(QStringLiteral("All books"));
    ASSERT_TRUE(v);
    v->setSortProperty(QStringLiteral("note.pages"), QStringLiteral("DESC"));
    YamlNode sizes = v->option(QStringLiteral("columnSize"));
    sizes.set(QStringLiteral("note.pages"), YamlNode::number(96));
    v->setOption(QStringLiteral("columnSize"), sizes);
    v->setSummary(QStringLiteral("note.pages"), QStringLiteral("Sum"));
    base.setDisplayName(QStringLiteral("note.pages"), QStringLiteral("Page count"));
    base.setFormula(QStringLiteral("title"), QStringLiteral("file.name + \": \" + author"));
    const QString expected = QStringLiteral(R"(filters:
  and:
    - file.hasTag("book")
    - status != "abandoned"
formulas:
  price_per_page: (price / pages).toFixed(2)
  age: (today() - date(published)).days
  title: 'file.name + ": " + author'
properties:
  note.author:
    displayName: Author
  formula.price_per_page:
    displayName: Price per page
  note.pages:
    displayName: Page count
views:
  - type: table
    name: All books
    order:
      - file.name
      - author
      - pages
      - formula.price_per_page
    sort:
      - property: pages
        direction: DESC
      - property: author
        direction: ASC
    summaries:
      pages: Sum
    columnSize:
      file.name: 220
      note.author: 180
      note.pages: 96
  - type: cards
    name: Gallery
    groupBy:
      property: status
      direction: DESC
    limit: 10
    image: note.cover
    cardSize: 180
)");
    EXPECT_EQ(base.toString(), expected);
    // and it reads back to the same model
    EXPECT_EQ(BaseFile::fromString(expected)->toString(), expected);
}

TEST(BasesFile, EmptyFileGetsTheDefaultTableView)
{
    const BaseFile base = *BaseFile::fromString(QString());
    ASSERT_EQ(base.views.size(), 1);
    EXPECT_EQ(base.views[0].type, QStringLiteral("table"));
    EXPECT_EQ(base.views[0].name, QStringLiteral("Table"));
    EXPECT_EQ(base.views[0].columns(), QStringList{QStringLiteral("file.name")});
    EXPECT_EQ(base.toString(), QStringLiteral("views:\n  - type: table\n    name: Table\n"));
    // "views: []" too
    EXPECT_EQ(BaseFile::fromString(QStringLiteral("views: []\n"))->views.size(), 1);
}

TEST(BasesFile, UnknownKeysAreKept)
{
    const QString text = QStringLiteral(R"(custom:
  nested:
    - 1
    - two
views:
  - type: board
    name: Kanban
    lanes:
      - todo
      - done
    wip: true
extra: 3
)");
    const BaseFile base = *BaseFile::fromString(text);
    EXPECT_EQ(base.views[0].type, QStringLiteral("board"));
    EXPECT_EQ(base.views[0].option(QStringLiteral("wip")).toBool(), true);
    // Obsidian writes the unknown top-level keys first
    EXPECT_EQ(base.toString(), QStringLiteral(R"(custom:
  nested:
    - 1
    - two
extra: 3
views:
  - type: board
    name: Kanban
    lanes:
      - todo
      - done
    wip: true
)"));
}

TEST(BasesFile, LegacyKeysMigrate)
{
    const QString text = QStringLiteral(R"(display:
  author: Writer
  file.name: Title
views:
  - type: table
    name: T
    sort:
      - column: author
        direction: DESC
      - property: pages
        direction: sideways
)");
    const BaseFile base = *BaseFile::fromString(text);
    EXPECT_EQ(base.displayName(QStringLiteral("note.author")), QStringLiteral("Writer"));
    EXPECT_EQ(base.displayName(QStringLiteral("file.name")), QStringLiteral("Title"));
    ASSERT_EQ(base.views[0].sortSpecs().size(), 1);
    EXPECT_EQ(base.views[0].sortSpecs()[0].property, QStringLiteral("note.author"));
    EXPECT_EQ(base.toString(), QStringLiteral(R"(properties:
  note.author:
    displayName: Writer
  file.name:
    displayName: Title
views:
  - type: table
    name: T
    sort:
      - property: author
        direction: DESC
)"));
}

TEST(BasesFile, FiltersParseAndSerialize)
{
    const QString text = QStringLiteral(R"(filters:
  or:
    - file.inFolder("Books")
    - and:
        - pages > 100
        - not:
            - status == "draft"
views:
  - type: list
    name: L
    filters: rating >= 4
)");
    const BaseFile base = *BaseFile::fromString(text);
    ASSERT_TRUE(base.filters);
    EXPECT_EQ(base.filters->kind, Filter::Kind::Or);
    EXPECT_EQ(base.filters->children[1].kind, Filter::Kind::And);
    EXPECT_EQ(base.filters->children[1].children[1].kind, Filter::Kind::Not);
    const QList<Filter::Rule> rules = base.filters->rules();
    ASSERT_EQ(rules.size(), 3);
    EXPECT_FALSE(rules[1].negated);
    EXPECT_TRUE(rules[2].negated);
    ASSERT_TRUE(base.views[0].filters);
    EXPECT_EQ(base.views[0].filters->text, QStringLiteral("rating >= 4"));
    EXPECT_EQ(base.toString(), text);
    const std::optional<Filter> both = Filter::both(base.filters, base.views[0].filters);
    ASSERT_TRUE(both);
    EXPECT_EQ(both->kind, Filter::Kind::And);
    EXPECT_EQ(both->children.size(), 2);
}

TEST(BasesFile, ErrorsUseObsidiansMessages)
{
    struct Bad {
        const char* text;
        const char* message;
    };
    const Bad cases[] = {
        {"views: 3\n", "Views must be an array"},
        {"views:\n  - 1\n", "\"view\" items must be a YAML object, however item 1 is not"},
        {"views:\n  - type: table\n", "Missing or invalid \"name\" in view 1"},
        {"views:\n  - name: A\n", "\"type\" is required in view \"A\""},
        {"views:\n  - name: A\n    type: [1]\n", "\"type\" must be a string in view \"A\""},
        {"views:\n  - name: A\n    type: table\n    order: x\n", "\"order\" must be a array in view \"A\""},
        {"views:\n  - name: A\n    type: table\n    groupBy: x\n", "\"groupBy\" must be a object in view \"A\""},
        {"views:\n  - name: A\n    type: table\n    filters: 3\n",
         "Unable to parse filters in view \"A\": \"filters\" must be a string or an object"},
        {"filters:\n  and: x\n", "\"and\" filter must be an array"},
        {"filters:\n  and: []\n  or: []\n", "\"filters\" may only have one of an \"and\", \"or\", or \"not\" keys."},
        {"formulas:\n  a: 1\n", "Formula values must be strings. Value for property a is not."},
        {"properties: 3\n", "Invalid properties configuration."},
        {"- a\n", "Query is an invalid format. It should be a YAML object."},
    };
    for (const Bad& c : cases) {
        QString error;
        EXPECT_FALSE(BaseFile::fromString(QString::fromUtf8(c.text), &error)) << c.text;
        EXPECT_EQ(error, QString::fromUtf8(c.message)) << c.text;
    }
    QString yamlError;
    EXPECT_FALSE(BaseFile::fromString(QStringLiteral("views: [\n"), &yamlError));
    EXPECT_FALSE(yamlError.isEmpty());
    // the YAML library's problem and position ("... at line L, column C")
    EXPECT_FALSE(BaseFile::fromString(QStringLiteral("views: [\n  - type: table\n"), &yamlError));
    EXPECT_EQ(yamlError, QStringLiteral("Illegal block entry at line 2, column 3"));
}

TEST(BasesFile, PropertyIds)
{
    EXPECT_EQ(propertyIdFromWritten(QStringLiteral("status")), QStringLiteral("note.status"));
    EXPECT_EQ(propertyIdFromWritten(QStringLiteral("file")), QStringLiteral("file.file"));
    EXPECT_EQ(propertyIdFromWritten(QStringLiteral("file.name")), QStringLiteral("file.name"));
    EXPECT_EQ(writtenPropertyId(QStringLiteral("note.status")), QStringLiteral("status"));
    EXPECT_EQ(writtenPropertyId(QStringLiteral("note.file.x")), QStringLiteral("note.file.x"));
    EXPECT_EQ(writtenPropertyId(QStringLiteral("file.file")), QStringLiteral("file"));
    EXPECT_EQ(writtenPropertyId(QStringLiteral("formula.x")), QStringLiteral("formula.x"));
    const PropertyRef r = parsePropertyId(QStringLiteral("my.prop"));
    EXPECT_EQ(r.type, QStringLiteral("note"));
    EXPECT_EQ(r.name, QStringLiteral("my.prop"));
}

TEST(BasesFile, SortPropertyToggles)
{
    BaseView v;
    v.setSortProperty(QStringLiteral("note.a"), QStringLiteral("TOGGLE"));
    EXPECT_EQ(v.sortSpecs()[0].direction, QStringLiteral("ASC"));
    v.setSortProperty(QStringLiteral("note.a"), QStringLiteral("TOGGLE"));
    EXPECT_EQ(v.sortSpecs()[0].direction, QStringLiteral("DESC"));
    v.setSortProperty(QStringLiteral("note.b"), QStringLiteral("ASC"));
    EXPECT_EQ(v.sortSpecs()[0].property, QStringLiteral("note.b"));
    v.setSortProperty(QStringLiteral("note.a"), QStringLiteral("TOGGLE"));
    ASSERT_EQ(v.sortSpecs().size(), 1);
    EXPECT_EQ(v.sortSpecs()[0].property, QStringLiteral("note.b"));
}

TEST(BasesFile, RemovingAFormulaCleansTheViews)
{
    BaseFile base = *BaseFile::fromString(QStringLiteral(R"(formulas:
  x: pages * 2
views:
  - type: table
    name: T
    groupBy:
      property: formula.x
      direction: ASC
    order:
      - file.name
      - formula.x
    sort:
      - property: formula.x
        direction: ASC
    summaries:
      formula.x: Sum
)"));
    base.removeFormula(QStringLiteral("x"));
    EXPECT_EQ(base.toString(), QStringLiteral(R"(views:
  - type: table
    name: T
    order:
      - file.name
    sort: []
    summaries: {}
)"));
}

TEST(BasesFile, YamlScalarsFollowTheCoreSchema)
{
    const std::optional<YamlNode> n = parseYaml(QStringLiteral(R"(a: 1
b: 1.5
c: true
d: null
e: "1"
f: 0x1F
g: .inf
h: 2024-01-01
i: ~
j: 'x'
k: 1e3
)"));
    ASSERT_TRUE(n);
    EXPECT_TRUE(n->value(QStringLiteral("a")).isNumber());
    EXPECT_EQ(n->value(QStringLiteral("b")).toNumber(), 1.5);
    EXPECT_TRUE(n->value(QStringLiteral("c")).isBool());
    EXPECT_TRUE(n->value(QStringLiteral("d")).isNull());
    EXPECT_TRUE(n->value(QStringLiteral("e")).isString());
    EXPECT_EQ(n->value(QStringLiteral("f")).toNumber(), 31);
    EXPECT_TRUE(std::isinf(n->value(QStringLiteral("g")).toNumber()));
    EXPECT_TRUE(n->value(QStringLiteral("h")).isString());
    EXPECT_TRUE(n->value(QStringLiteral("i")).isNull());
    EXPECT_TRUE(n->value(QStringLiteral("j")).isString());
    EXPECT_EQ(n->value(QStringLiteral("k")).toNumber(), 1000);
    // written back: strings that would read as something else are quoted
    YamlNode m = YamlNode::map();
    m.set(QStringLiteral("s1"), YamlNode::string(QStringLiteral("1")));
    m.set(QStringLiteral("s2"), YamlNode::string(QStringLiteral("true")));
    m.set(QStringLiteral("s3"), YamlNode::string(QStringLiteral("a: b")));
    m.set(QStringLiteral("s4"), YamlNode::string(QStringLiteral("#tag")));
    m.set(QStringLiteral("n"), YamlNode());
    m.set(QStringLiteral("l"), YamlNode::list());
    m.set(QStringLiteral("o"), YamlNode::map());
    EXPECT_EQ(dumpYaml(m), QStringLiteral("s1: \"1\"\ns2: \"true\"\ns3: \"a: b\"\ns4: \"#tag\"\nn:\nl: []\no: {}\n"));
}

} // namespace
