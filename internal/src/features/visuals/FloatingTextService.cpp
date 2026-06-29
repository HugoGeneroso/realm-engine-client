#include "pch-il2cpp.h"
#include "DbgFileLog.h"
#include <mutex>
#include <cstring>
#include "FloatingTextService.h"

namespace {
    std::mutex s_pluginFloatingTextMutex;
    char s_pluginFloatingText[128] = {};
    bool s_pluginFloatingTextPending = false;
}

void FloatingTextService::QueuePluginText(const char* text)
{
    std::lock_guard<std::mutex> lk(s_pluginFloatingTextMutex);
    strncpy_s(s_pluginFloatingText, sizeof(s_pluginFloatingText), text ? text : "", _TRUNCATE);
    s_pluginFloatingTextPending = true;
}

void FloatingTextService::ApplyPendingPluginText()
{
    // In-game floating text via IL2CPP on the dPresent thread caused freezes/crashes
    // when toggling features (ShowFloatingText / FindObjectsOfType). The dashboard
    // already shows enable/disable state — drop the pending notification here.
    char text[128] = {};
    {
        std::lock_guard<std::mutex> lk(s_pluginFloatingTextMutex);
        if (!s_pluginFloatingTextPending) return;
        strncpy_s(text, sizeof(text), s_pluginFloatingText, _TRUNCATE);
        s_pluginFloatingTextPending = false;
    }
    DBG_FILE_LOG("[FloatingText] dropped (dashboard-only) text=" << text);
}
