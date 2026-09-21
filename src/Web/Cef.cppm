module;

#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_browser_process_handler.h"
#include "include/cef_client.h"
#include "include/cef_command_line.h"
#include "include/cef_display_handler.h"
#include "include/cef_frame.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_render_handler.h"
#include "include/cef_render_process_handler.h"
#include "include/cef_v8.h"
#if __is_target_os(macos)
#    include "include/wrapper/cef_library_loader.h"
#endif

export module weweb:cef;

export using ::cef_color_type_t;
export using ::cef_key_event_type_t;
export using ::cef_log_severity_t;
export using ::cef_mouse_button_type_t;
export using ::CefProcessId;
export using ::cef_string_t;
export using ::CEF_COLOR_TYPE_BGRA_8888;
export using ::CEF_COLOR_TYPE_RGBA_8888;
export using ::EVENTFLAG_LEFT_MOUSE_BUTTON;
export using ::LOGSEVERITY_DEBUG;
export using ::LOGSEVERITY_ERROR;
export using ::LOGSEVERITY_FATAL;
export using ::LOGSEVERITY_INFO;
export using ::LOGSEVERITY_VERBOSE;
export using ::LOGSEVERITY_WARNING;
export using ::PET_VIEW;
export using ::PET_POPUP;
export using ::PID_BROWSER;
export using ::PID_RENDERER;
export using ::V8_PROPERTY_ATTRIBUTE_DONTDELETE;

export using ::CefAcceleratedPaintInfo;
export using ::CefApp;
export using ::CefBaseRefCounted;
export using ::CefBrowser;
export using ::CefBrowserHost;
export using ::CefBrowserProcessHandler;
export using ::CefBrowserSettings;
export using ::CefClient;
export using ::CefCommandLine;
export using ::CefDisplayHandler;
export using ::CefExecuteProcess;
export using ::CefDoMessageLoopWork;
export using ::CefFrame;
export using ::CefInitialize;
export using ::CefKeyEvent;
export using ::CefLifeSpanHandler;
export using ::CefLoadHandler;
export using ::CefMainArgs;
export using ::CefMouseEvent;
export using ::CefProcessMessage;
export using ::CefRect;
export using ::CefRefCount;
export using ::CefRefPtr;
export using ::CefRenderHandler;
export using ::CefRenderProcessHandler;
export using ::CefScreenInfo;
export using ::CefSettings;
export using ::CefShutdown;
export using ::CefString;
export using ::CefV8Context;
export using ::CefV8Handler;
export using ::CefV8Value;
export using ::CefV8ValueList;
export using ::CefWindowInfo;
#if __is_target_os(macos)
export using ::cef_load_library;
export using ::cef_unload_library;
#endif
