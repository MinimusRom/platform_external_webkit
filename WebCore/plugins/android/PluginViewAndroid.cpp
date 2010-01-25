/*
 * Copyright 2009, The Android Open Source Project
 * Copyright (C) 2006, 2007 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "WebCore"

#include "config.h"
#include "PluginView.h"

#include "Document.h"
#include "Element.h"
#include "EventNames.h"
#include "FocusController.h"
#include "FrameLoader.h"
#include "FrameLoadRequest.h"
#include "FrameTree.h"
#include "Frame.h"
#include "FrameView.h"
#include "GraphicsContext.h"
#include "HTMLNames.h"
#include "HTMLPlugInElement.h"
#include "Image.h"
#include "KeyboardEvent.h"
#include "MIMETypeRegistry.h"
#include "MouseEvent.h"
#include "NetworkStateNotifier.h"
#include "NotImplemented.h"
#include "Page.h"
#include "PlatformGraphicsContext.h"
#include "PlatformKeyboardEvent.h"
#include "PluginMainThreadScheduler.h"
#include "PluginPackage.h"
#include "Touch.h"
#include "TouchEvent.h"
#include "TouchList.h"
#include "android_graphics.h"
#include "SkCanvas.h"
#include "npruntime_impl.h"
// #include "runtime_root.h"
#include "utils/SystemClock.h"
#include "ScriptController.h"
#include "Settings.h"

#if USE(JSC)
#include <runtime/JSLock.h>
#endif

#include <wtf/ASCIICType.h>
// #include "runtime.h"
#include "WebViewCore.h"

#include "PluginDebug.h"
#include "PluginDebugAndroid.h"
#include "PluginViewBridgeAndroid.h"
#include "PluginWidgetAndroid.h"

#include "android_npapi.h"
#include "ANPSurface_npapi.h"
#include "ANPSystem_npapi.h"
#include "SkANP.h"
#include "SkFlipPixelRef.h"

///////////////////////////////////////////////////////////////////////////////

extern void ANPAudioTrackInterfaceV0_Init(ANPInterface* value);
extern void ANPBitmapInterfaceV0_Init(ANPInterface* value);
extern void ANPCanvasInterfaceV0_Init(ANPInterface* value);
extern void ANPEventInterfaceV0_Init(ANPInterface* value);
extern void ANPLogInterfaceV0_Init(ANPInterface* value);
extern void ANPMatrixInterfaceV0_Init(ANPInterface* value);
extern void ANPOffscreenInterfaceV0_Init(ANPInterface* value);
extern void ANPPaintInterfaceV0_Init(ANPInterface* value);
extern void ANPPathInterfaceV0_Init(ANPInterface* value);
extern void ANPSurfaceInterfaceV0_Init(ANPInterface* value);
extern void ANPTypefaceInterfaceV0_Init(ANPInterface* value);
extern void ANPWindowInterfaceV0_Init(ANPInterface* value);
extern void ANPSystemInterfaceV0_Init(ANPInterface* value);

struct VarProcPair {
    int         enumValue;
    size_t      size;
    void        (*proc)(ANPInterface*);
};

#define VARPROCLINE(name)   \
    k##name##_ANPGetValue, sizeof(ANP##name), ANP##name##_Init

static const VarProcPair gVarProcs[] = {
    { VARPROCLINE(AudioTrackInterfaceV0)    },
    { VARPROCLINE(BitmapInterfaceV0)        },
    { VARPROCLINE(CanvasInterfaceV0)        },
    { VARPROCLINE(EventInterfaceV0)         },
    { VARPROCLINE(LogInterfaceV0)           },
    { VARPROCLINE(MatrixInterfaceV0)        },
    { VARPROCLINE(PaintInterfaceV0)         },
    { VARPROCLINE(PathInterfaceV0)          },
    { VARPROCLINE(SurfaceInterfaceV0)       },
    { VARPROCLINE(TypefaceInterfaceV0)      },
    { VARPROCLINE(WindowInterfaceV0)        },
    { VARPROCLINE(SystemInterfaceV0)        },
};

/*  return true if var was an interface request (error will be set accordingly)
    return false if var is not a recognized interface (and ignore error param)
 */
static bool anp_getInterface(NPNVariable var, void* value, NPError* error) {
    const VarProcPair* iter = gVarProcs;
    const VarProcPair* stop = gVarProcs + SK_ARRAY_COUNT(gVarProcs);
    while (iter < stop) {
        if (iter->enumValue == var) {
            ANPInterface* i = reinterpret_cast<ANPInterface*>(value);
            if (i->inSize < iter->size) {
                SkDebugf("------- interface %d, expected size %d, allocated %d\n",
                         var, iter->size, i->inSize);
                *error = NPERR_INCOMPATIBLE_VERSION_ERROR;
            } else {
                iter->proc(i);
                *error = NPERR_NO_ERROR;
            }
            return true;
        }
        iter += 1;
    }
    SkDebugf("------ unknown NPNVariable %d\n", var);
    return false;
}

///////////////////////////////////////////////////////////////////////////////

using std::min;

using namespace WTF;

namespace WebCore {

using namespace HTMLNames;

void PluginView::platformInit()
{
    setPlatformWidget(new PluginViewBridgeAndroid());

    m_isWindowed = false;   // we don't support windowed yet

    m_window = new PluginWidgetAndroid(this);

    m_npWindow.type = NPWindowTypeDrawable;
    m_npWindow.window = 0;
}

bool PluginView::platformStart()
{
    return true;
}

void PluginView::platformDestroy()
{
    delete m_window;
}

void PluginView::handleTouchEvent(TouchEvent* event)
{
    if (!m_window->isAcceptingEvent(kTouch_ANPEventFlag))
        return;

    ANPEvent evt;
    SkANP::InitEvent(&evt, kTouch_ANPEventType);

    bool ignoreRet = false;
    const AtomicString& type = event->type();
    if (eventNames().touchstartEvent == type)
        evt.data.touch.action = kDown_ANPTouchAction;
    else if (eventNames().touchendEvent == type)
        evt.data.touch.action = kUp_ANPTouchAction;
    else if (eventNames().touchmoveEvent == type)
        evt.data.touch.action = kMove_ANPTouchAction;
    else if (eventNames().touchcancelEvent == type)
        evt.data.touch.action = kCancel_ANPTouchAction;
    else if (eventNames().touchlongpressEvent == type) {
        evt.data.touch.action = kLongPress_ANPTouchAction;
        ignoreRet = true;
    } else if (eventNames().touchdoubletapEvent == type) {
        evt.data.touch.action = kDoubleTap_ANPTouchAction;
        ignoreRet = true;
    } else
        return;

    evt.data.touch.modifiers = 0;   // todo

    // In the event of a touchend (up) event, we must ask the changedTouch for the
    // co-ordinates as there is no touch in touches anymore.
    TouchList* touches = (evt.data.touch.action == kUp_ANPTouchAction) ?
                              event->changedTouches() : event->touches();

    // Convert to coordinates that are relative to the plugin.
    // We only support single touch points at the moment, so we want to look at index 0 only.
    IntPoint localPos = roundedIntPoint(m_element->renderer()->absoluteToLocal(IntPoint(touches->item(0)->pageX(), touches->item(0)->pageY())));
    evt.data.touch.x = localPos.x();
    evt.data.touch.y = localPos.y();

    int16 ret = m_window->sendEvent(evt);
    if (ignoreRet)
        return;
    if (ret & kHandleTouch_ANPTouchResult) {
        // The plugin needs focus to receive keyboard events
        if (evt.data.touch.action == kDown_ANPTouchAction) {
            if (Page* page = m_parentFrame->page())
                page->focusController()->setFocusedFrame(m_parentFrame);
            m_parentFrame->document()->setFocusedNode(m_element);
        }
        event->preventDefault();
    } else {
        if (ret & kHandleLongPress_ANPTouchResult)
            event->preventLongPress();
        if (ret & kHandleDoubleTap_ANPTouchResult)
            event->preventDoubleTap();
    }
}

void PluginView::handleMouseEvent(MouseEvent* event)
{
    const AtomicString& type = event->type();
    bool isUp = (eventNames().mouseupEvent == type);
    bool isDown = (eventNames().mousedownEvent == type);

    ANPEvent    evt;

    if (isUp || isDown) {
        SkANP::InitEvent(&evt, kMouse_ANPEventType);
        evt.data.mouse.action = isUp ? kUp_ANPMouseAction : kDown_ANPMouseAction;

        // Convert to coordinates that are relative to the plugin.
        IntPoint localPos = roundedIntPoint(m_element->renderer()->absoluteToLocal(event->absoluteLocation()));
        evt.data.mouse.x = localPos.x();
        evt.data.mouse.y = localPos.y();

        if (isDown) {
            // The plugin needs focus to receive keyboard events
            if (Page* page = m_parentFrame->page())
                page->focusController()->setFocusedFrame(m_parentFrame);
            m_parentFrame->document()->setFocusedNode(m_element);
        }
    }
    else {
      return;
    }

    if (m_window->sendEvent(evt)) {
        event->setDefaultHandled();
    }
}

static ANPKeyModifier make_modifiers(bool shift, bool alt) {
    ANPKeyModifier mod = 0;
    if (shift) {
        mod |= kShift_ANPKeyModifier;
    }
    if (alt) {
        mod |= kAlt_ANPKeyModifier;
    }
    return mod;
}

void PluginView::handleFocusEvent(bool hasFocus)
{
    ANPEvent evt;
    SkANP::InitEvent(&evt, kLifecycle_ANPEventType);
    evt.data.lifecycle.action = hasFocus ? kGainFocus_ANPLifecycleAction :
                                           kLoseFocus_ANPLifecycleAction;
    m_window->sendEvent(evt);

    // redraw the plugin which subsequently invalidates the nav cache
    IntRect rect = IntRect(m_npWindow.x, m_npWindow.y,
                           m_npWindow.width, m_npWindow.height);
    m_window->webViewCore()->contentInvalidate(rect);
}

void PluginView::handleKeyboardEvent(KeyboardEvent* event)
{
    if (!m_window->isAcceptingEvent(kKey_ANPEventFlag))
        return;

    const PlatformKeyboardEvent* pke = event->keyEvent();
    if (NULL == pke) {
        return;
    }

    bool ignoreEvent = false;

    ANPEvent evt;
    SkANP::InitEvent(&evt, kKey_ANPEventType);

    switch (pke->type()) {
        case PlatformKeyboardEvent::KeyDown:
#ifdef TRACE_KEY_EVENTS
            SkDebugf("--------- KeyDown, ignore\n");
#endif
            ignoreEvent = true;
            break;
        case PlatformKeyboardEvent::RawKeyDown:
            evt.data.key.action = kDown_ANPKeyAction;
            break;
        case PlatformKeyboardEvent::Char:
#ifdef TRACE_KEY_EVENTS
            SkDebugf("--------- Char, ignore\n");
#endif
            ignoreEvent = true;
            break;
        case PlatformKeyboardEvent::KeyUp:
            evt.data.key.action = kUp_ANPKeyAction;
            break;
        default:
#ifdef TRACE_KEY_EVENTS
            SkDebugf("------ unexpected keyevent type %d\n", pke->type());
#endif
            ignoreEvent = true;
            break;
    }

    /* the plugin should be the only party able to return nav control to the
     * browser UI. Therefore, if we discard an event on behalf of the plugin
     * we should mark the event as being handled.
     */
    if (ignoreEvent) {
        int keyCode = pke->nativeVirtualKeyCode();
        if (keyCode >= kDpadUp_ANPKeyCode && keyCode <= kDpadCenter_ANPKeyCode)
            event->setDefaultHandled();
        return;
    }

    evt.data.key.nativeCode = pke->nativeVirtualKeyCode();
    evt.data.key.virtualCode = pke->windowsVirtualKeyCode();
    evt.data.key.repeatCount = pke->repeatCount();
    evt.data.key.modifiers = make_modifiers(pke->shiftKey(), pke->altKey());
    evt.data.key.unichar = pke->unichar();

    if (m_window->sendEvent(evt)) {
        event->setDefaultHandled();
    } else if (m_window->inFullScreen()){
        // while in the full screen mode, always consumes the key events and
        // keeps the document focus
        event->setDefaultHandled();
    } else {
        // remove the plugin from the document's focus
        m_parentFrame->document()->focusedNodeRemoved();
    }
}

NPError PluginView::handlePostReadFile(Vector<char>& buffer, uint32 len, const char* buf)
{
    notImplemented();
    return NPERR_GENERIC_ERROR;
}

NPError PluginView::getValueStatic(NPNVariable variable, void* value)
{
    // our interface query is valid with no NPP instance
    NPError error = NPERR_GENERIC_ERROR;
    if ((value != NULL) && (variable == NPNVisOfflineBool)) {
      bool* retValue = static_cast<bool*>(value);
      *retValue = !networkStateNotifier().onLine();
      return NPERR_NO_ERROR;
    }
    (void)anp_getInterface(variable, value, &error);
    return error;
}

void PluginView::setParent(ScrollView* parent)
{
    Widget::setParent(parent);

    if (parent) {
        // the widget needs initialized now so that the plugin has access to
        // WebViewCore when NPP_New is called
        if (m_window && !m_window->webViewCore()) {
            android::WebViewCore* c = android::WebViewCore::getWebViewCore(this->parent());
            m_window->init(c);
        }
        init();
    }
}

void PluginView::setNPWindowRect(const IntRect&)
{
    setNPWindowIfNeeded();
}

void PluginView::setNPWindowIfNeeded()
{
    if (!m_isStarted || !parent())
        return;

    // in Android, plugin always get the setwindow() in the page coordinate.
    IntRect pageRect = m_windowRect;
    ScrollView* top = parent();
    while (top->parent())
        top = top->parent();
    // only the top ScrollView can have the offset
    pageRect.move(top->scrollOffset());

    // the m_npWindow is relative to the page
    m_npWindow.x = pageRect.x();
    m_npWindow.y = pageRect.y();
    m_npWindow.width = pageRect.width();
    m_npWindow.height = pageRect.height();

    m_npWindow.clipRect.left = pageRect.x();
    m_npWindow.clipRect.top = pageRect.y();
    m_npWindow.clipRect.right = pageRect.x() + pageRect.width();
    m_npWindow.clipRect.bottom = pageRect.y() + pageRect.height();

    if (m_plugin->pluginFuncs()->setwindow) {
#if USE(JSC)
        JSC::JSLock::DropAllLocks dropAllLocks(false);
#endif
        setCallingPlugin(true);
        m_plugin->pluginFuncs()->setwindow(m_instance, &m_npWindow);
        setCallingPlugin(false);
    }

    m_window->setWindow(&m_npWindow, m_isTransparent);
}

NPError PluginView::getValue(NPNVariable variable, void* value)
{
    switch (variable) {
        case NPNVWindowNPObject: {
            NPObject* windowScriptObject =
                    m_parentFrame->script()->windowScriptNPObject();

            // Return value is expected to be retained, as described
            // here:
            // <http://www.mozilla.org/projects/plugin/npruntime.html>
            if (windowScriptObject)
                _NPN_RetainObject(windowScriptObject);

            void** v = (void**)value;
            *v = windowScriptObject;

            return NPERR_NO_ERROR;
        }

        case NPNVPluginElementNPObject: {
            NPObject* pluginScriptObject = 0;

            if (m_element->hasTagName(appletTag) ||
                m_element->hasTagName(embedTag) ||
                m_element->hasTagName(objectTag)) {
                HTMLPlugInElement* pluginElement =
                        static_cast<HTMLPlugInElement*>(m_element);
                pluginScriptObject = pluginElement->getNPObject();
            }

            // Return value is expected to be retained, as described
            // here:
            // <http://www.mozilla.org/projects/plugin/npruntime.html>
            if (pluginScriptObject)
                _NPN_RetainObject(pluginScriptObject);

            void** v = (void**)value;
            *v = pluginScriptObject;

            return NPERR_NO_ERROR;
        }

        case NPNVnetscapeWindow: {
            // Return the top level WebView Java object associated
            // with this instance.
            jobject *retObject = static_cast<jobject*>(value);
            *retObject = android::WebViewCore::getWebViewCore(parent())->getWebViewJavaObject();
            return NPERR_NO_ERROR;
        }

        case NPNVisOfflineBool: {
            if (value == NULL) {
              return NPERR_GENERIC_ERROR;
            }
            bool* retValue = static_cast<bool*>(value);
            *retValue = !networkStateNotifier().onLine();
            return NPERR_NO_ERROR;
        }

        case kSupportedDrawingModel_ANPGetValue: {
            uint32_t* bits = reinterpret_cast<uint32_t*>(value);
            *bits = kBitmap_ANPDrawingModel & kSurface_ANPDrawingModel;
            return NPERR_NO_ERROR;
        }

        case kJavaContext_ANPGetValue: {
            jobject* retObject = static_cast<jobject*>(value);
            *retObject = android::WebViewCore::getWebViewCore(parent())->getContext();
            return NPERR_NO_ERROR;
        }

        default: {
            NPError error = NPERR_GENERIC_ERROR;
            (void)anp_getInterface(variable, value, &error);
            return error;
        }
    }
}

NPError PluginView::platformSetValue(NPPVariable variable, void* value)
{
    NPError error = NPERR_GENERIC_ERROR;

    switch (variable) {
        case kRequestDrawingModel_ANPSetValue: {
            ANPDrawingModel model = reinterpret_cast<ANPDrawingModel>(value);
            if (m_window->setDrawingModel(model))
                error = NPERR_NO_ERROR;
            break;
        }
        case kAcceptEvents_ANPSetValue : {
            if(value) {
                ANPEventFlags flags = *reinterpret_cast<ANPEventFlags*>(value);
                m_window->updateEventFlags(flags);
                error = NPERR_NO_ERROR;
            }
            break;
        }
        default:
            break;
    }
    return error;
}

void PluginView::invalidateRect(const IntRect& r)
{
    m_window->inval(r, true);
}

void PluginView::invalidateRect(NPRect* rect)
{
    IntRect r;

    if (rect) {
        r = IntRect(rect->left, rect->top,
                    rect->right - rect->left, rect->bottom - rect->top);
    } else {
        r = IntRect(0, 0, m_npWindow.width, m_npWindow.height);
    }

    m_window->inval(r, true);
}

void PluginView::invalidateRegion(NPRegion region)
{
    // we don't support/define regions (yet), so do nothing
}

void PluginView::forceRedraw()
{
    this->invalidateRect(0);
}

void PluginView::setFocus()
{
    Widget::setFocus();
//    SkDebugf("------------- setFocus %p\n", this);
}

void PluginView::show()
{
    setSelfVisible(true);
    Widget::show();
}

void PluginView::hide()
{
    setSelfVisible(false);
    Widget::hide();
}

void PluginView::paint(GraphicsContext* context, const IntRect& rect)
{
    if (!m_isStarted) {
        // Draw the "missing plugin" image
        paintMissingPluginIcon(context, rect);
        return;
    }

    IntRect frame = frameRect();
    if (!frame.width() || !frame.height()) {
        return;
    }

    m_window->inval(rect, false);
    context->save();
    context->translate(frame.x(), frame.y());
    m_window->draw(android_gc2canvas(context));
    context->restore();
}

void PluginView::updatePluginWidget()
{
    FrameView* frameView = static_cast<FrameView*>(parent());
    if (frameView) {
        IntRect oldWindowRect = m_windowRect;

        m_windowRect = frameView->contentsToWindow(frameRect());

        if (m_windowRect != oldWindowRect)
            setNPWindowIfNeeded();
    }
}

// new as of SVN 38068, Nov 5 2008
void PluginView::setParentVisible(bool) {
    notImplemented();
}

void PluginView::halt() {
    notImplemented();
}

void PluginView::restart() {
    notImplemented();
}

} // namespace WebCore
