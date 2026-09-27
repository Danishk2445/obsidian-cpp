// gtest entry point for tests that need a QApplication (fonts, painting, widgets). Runs offscreen.
#include <QApplication>
#include <QThreadPool>
#include <gtest/gtest.h>

int main(int argc, char** argv)
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
#if defined(__SANITIZE_THREAD__)
    // Pool threads that expire and get replaced can reuse a pthread id before ThreadSanitizer's registry has
    // forgotten it ("CHECK failed: sanitizer_thread_registry.cpp ... live_.try_emplace"): keep them.
    QThreadPool::globalInstance()->setExpiryTimeout(-1);
#endif
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
