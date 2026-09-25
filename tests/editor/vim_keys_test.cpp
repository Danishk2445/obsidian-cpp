// Vim key sequences: initial text + cursor, keys, expected text + cursor/selection, mode and
// registers. The expectations come from an oracle: CodeMirror 6 with the vim.js Obsidian 1.13 ships
// (lib/codemirror/vim.js) run in jsdom, fed the same keys — except where this implementation
// deliberately follows upstream CodeMirror-vim/vim instead of an Obsidian bug (noted in
// vim_cases.inc's generator and in docs). Typing goes through the editor's real input path, with
// auto-pairing off (the oracle has none).
#include "editor/vim_test_support.h"

#include "editor/vim/vim_engine.h"

using namespace oc;
using namespace oc::test;

namespace {

struct VimCase {
    const char* name;
    const char* text;
    const char* keys;
    const char* expect;
    const char* mode;
    std::vector<std::pair<const char*, const char*>> regs;
    const char* ranges;
    const char* clipboard;
};

const std::vector<VimCase>& cases()
{
    static const std::vector<VimCase> c = {
#include "editor/vim_cases.inc"
    };
    return c;
}

VimHarness* g_harness = nullptr;

class VimKeys : public ::testing::TestWithParam<size_t> {
protected:
    static void SetUpTestSuite()
    {
        static ThemeManager themes;
        themes.apply();
        mdcmd::settings().autoPairBrackets = false;
        mdcmd::settings().autoPairMarkdown = false;
        g_harness = new VimHarness;
    }
    static void TearDownTestSuite()
    {
        delete g_harness;
        g_harness = nullptr;
        mdcmd::settings() = mdcmd::Settings();
    }
};

TEST_P(VimKeys, Sequence)
{
    const VimCase& c = cases()[GetParam()];
    VimHarness& h = *g_harness;
    VimController::resetGlobalState();
    vim::Clipboard cb;
    cb.read = [&h] { return h.clipboard; };
    cb.write = [&h](const QString& t) { h.clipboard = t; };
    VimController::setClipboard(cb);
    h.clipboard = c.clipboard ? QString::fromUtf8(c.clipboard) : QString();
    h.open(QString::fromUtf8(c.text));
    h.keys(QString::fromUtf8(c.keys));
    SCOPED_TRACE(std::string("keys: ") + c.keys);
    EXPECT_EQ(h.marked().toStdString(), std::string(c.expect));
    EXPECT_EQ(h.vim->modeName().toStdString(), std::string(c.mode));
    if (c.ranges) {
        EXPECT_EQ(h.ranges().toStdString(), std::string(c.ranges));
    }
    for (const auto& [name, text] : c.regs)
        EXPECT_EQ(vim::VimGlobal::instance().registerController.get(QString::fromUtf8(name)).toString().toStdString(), std::string(text))
            << "register " << name;
    // leave normal mode behind for the next case
    if (h.vim->panel()->isPromptOpen())
        h.vim->panel()->closePrompt();
}

std::string caseName(const ::testing::TestParamInfo<size_t>& info)
{
    std::string n = std::to_string(info.param) + "_";
    for (const char* p = cases()[info.param].name; *p; ++p)
        n += std::isalnum(static_cast<unsigned char>(*p)) ? *p : '_';
    return n;
}

INSTANTIATE_TEST_SUITE_P(Oracle, VimKeys, ::testing::Range<size_t>(0, cases().size()), caseName);

} // namespace
