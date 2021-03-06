// Copyright (c) 2013 Stanislas Polu.
// See the LICENSE file.

#include "exo_browser/src/browser/exo_frame.h"

#include "base/bind.h"
#include "base/base64.h"
#include "base/message_loop/message_loop.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/gfx/codec/png_codec.h"
#include "content/public/common/favicon_url.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/render_widget_host_view.h" 
#include "content/public/browser/render_view_host.h" 
#include "content/public/browser/web_contents_view.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_source.h"
#include "content/public/browser/notification_types.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_http_handler.h"
#include "content/public/browser/devtools_manager.h"
#include "exo_browser/src/browser/content_browser_client.h"
#include "exo_browser/src/browser/browser_main_parts.h"
#include "exo_browser/src/browser/exo_browser.h"
#include "exo_browser/src/browser/content_browser_client.h"
#include "exo_browser/src/browser/util/renderer_preferences_util.h"
#include "exo_browser/src/devtools/devtools_delegate.h"
#include "exo_browser/src/node/node_thread.h"
#include "exo_browser/src/node/api/exo_frame_wrap.h"

using namespace content;

namespace exo_browser {

static std::map<WebContents*, ExoFrame*> s_exo_frames;

ExoFrame* ExoFrame::ExoFrameForWebContents(
    content::WebContents* web_contents)
{
  return s_exo_frames[web_contents];
}


ExoFrame::ExoFrame(
    const std::string& name,
    content::WebContents* web_contents,
    ExoFrameWrap* wrapper)
: WebContentsObserver(web_contents),
  name_(name),
  type_(NOTYPE_FRAME),
  wrapper_(wrapper)
{
  /* TODO(spolu): Check ExoSession herited from source WebContents */
  web_contents_.reset(web_contents);
  WebContentsObserver::Observe(web_contents);
  LOG(INFO) << "ExoFrame Constructor [" << web_contents << "]";
  Init();
}

ExoFrame::ExoFrame(
    const std::string& name,
    ExoSession* session,
    ExoFrameWrap* wrapper)
: name_(name),
  type_(NOTYPE_FRAME),
  wrapper_(wrapper)
{
  WebContents::CreateParams create_params((BrowserContext*)session);
  WebContents* web_contents = WebContents::Create(create_params);
  web_contents_.reset(web_contents);
  WebContentsObserver::Observe(web_contents);
  LOG(INFO) << "ExoFrame Constructor (web_contents created) [" 
            << web_contents << "]";
  Init();
}

void
ExoFrame::Init()
{
  s_exo_frames[web_contents_.get()] = this;

  renderer_preferences_util::UpdateFromSystemSettings(
      web_contents_->GetMutableRendererPrefs());
  web_contents_->GetRenderViewHost()->SyncRendererPrefs();
}

void
ExoFrame::SetParent(
    ExoBrowser* parent)
{
  LOG(INFO) << "ExoFrame SetParent [" << web_contents_ << "]: " << parent;
  parent_ = parent;
  web_contents_->SetDelegate(parent_);
}

void
ExoFrame::SetType(
    FRAME_TYPE type)
{
  type_ = type;
}


ExoFrame::~ExoFrame()
{
  /* Not much to do. If we're here that means that our JS wrapper has been */
  /* reclaimed and that most of what is related to us has been destroyed   */
  /* already. Our associated web_contents, should be deleted with its      */
  /* scoped_ptr.                                                           */
  LOG(INFO) << "ExoFrame Destructor [" << web_contents_ << "]";
  s_exo_frames.erase(web_contents_.get());
}

void
ExoFrame::LoadURL(
    const GURL& url)
{
  NavigationController::LoadURLParams params(url);
  /* TODO(spolu): Manage `transition_type` */
  params.transition_type = PageTransitionFromInt(
      PAGE_TRANSITION_TYPED | PAGE_TRANSITION_FROM_ADDRESS_BAR);
  web_contents_->GetController().LoadURLWithParams(params);
}

void
ExoFrame::GoBackOrForward(int offset)
{
  web_contents_->GetController().GoToOffset(offset);
}

void 
ExoFrame::Reload() 
{
  web_contents_->GetController().Reload(true);
}

void 
ExoFrame::Stop() 
{
  web_contents_->Stop();
}

void
ExoFrame::Focus()
{
  web_contents_->GetView()->Focus();
}

void
ExoFrame::Find(
    int request_id, 
    const string16& search_text,
    const WebKit::WebFindOptions& options)
{
  web_contents_->GetRenderViewHost()->Find(request_id, search_text, options);
}

void
ExoFrame::StopFinding(
    StopFindAction action)
{
  web_contents_->GetRenderViewHost()->StopFinding(action);
}

void
ExoFrame::CaptureFrame(
    const base::Callback<void(bool, const std::string&)>& callback)
{
  /* CaptureFrameImpl is RefCounted so it will get deallocated when needed. */
  (new CaptureFrameImpl(this, callback))->Run();
}

void
ExoFrame::Zoom(
    content::PageZoom zoom)
{
  web_contents_->GetRenderViewHost()->Zoom(zoom);
}

double
ExoFrame::ZoomLevel() const
{
  return web_contents_->GetZoomLevel();
}

const GURL
ExoFrame::GetDevTools()
{
  ExoBrowserContentBrowserClient* browser_client = 
    ExoBrowserContentBrowserClient::Get();
  
  RenderViewHost* inspected_rvh = web_contents()->GetRenderViewHost();
  scoped_refptr<DevToolsAgentHost> agent(
      DevToolsAgentHost::GetOrCreateFor(inspected_rvh));
  DevToolsManager* manager = DevToolsManager::GetInstance();

  if(agent->IsAttached()) {
    /* Break remote debugging debugging session. */
    manager->CloseAllClientHosts();
  }

  ExoBrowserDevToolsDelegate* delegate = 
    browser_client->browser_main_parts()->devtools_delegate(); 

  GURL url = delegate->devtools_http_handler()->GetFrontendURL(agent.get());
  return url;
}

ExoFrame::CaptureFrameImpl::CaptureFrameImpl(
    ExoFrame* parent,
    const base::Callback<void(bool, const std::string&)>& callback)
  : parent_(parent),
    callback_(callback)
{
}

ExoFrame::CaptureFrameImpl::~CaptureFrameImpl()
{
}

void
ExoFrame::CaptureFrameImpl::Run()
{
  RenderViewHost* render_view_host = 
    parent_->web_contents_->GetRenderViewHost();
  content::RenderWidgetHostView* view = render_view_host->GetView();
  if(!view) {
    callback_.Run(false, "");
    return;
  }

  render_view_host->CopyFromBackingStore(
      gfx::Rect(),
      view->GetViewBounds().size(),
      base::Bind(&ExoFrame::CaptureFrameImpl::CopyFromBackingStoreComplete, 
                 this));
}

void
ExoFrame::CaptureFrameImpl::CopyFromBackingStoreComplete(
    bool succeeded,
    const SkBitmap& bitmap)
{
  if(succeeded) {
    Finish(succeeded, bitmap);
    return;
  }

  content::RenderWidgetHost* render_widget_host = 
    parent_->web_contents_->GetRenderViewHost();
  if(!render_widget_host) {
    callback_.Run(false, "");
    return;
  }

  render_widget_host->GetSnapshotFromRenderer(
      gfx::Rect(),
      base::Bind(&ExoFrame::CaptureFrameImpl::Finish, this));
}

void
ExoFrame::CaptureFrameImpl::Finish(
    bool succeeded,
    const SkBitmap& screen_capture) {
  if(!succeeded) {
    callback_.Run(false, "");
    return;
  }

  std::vector<unsigned char> data;
  SkAutoLockPixels screen_capture_lock(screen_capture);
  bool encoded = gfx::PNGCodec::EncodeBGRASkBitmap(screen_capture, true, &data);
  std::string mime_type = "image/png";

  if(!encoded) {
    callback_.Run(false, "");
    return;
  }

  std::string base64_result;
  base::StringPiece stream_as_string(
      reinterpret_cast<const char*>(vector_as_array(&data)), data.size());
  base::Base64Encode(stream_as_string, &base64_result);
  base64_result.insert(0, base::StringPrintf("data:%s;base64,",
                                             mime_type.c_str()));
  callback_.Run(true, base64_result);
}



/******************************************************************************/
/*                    WEBCONTENTSOBSERVER IMPLEMENTATION                      */
/******************************************************************************/
void 
ExoFrame::DidUpdateFaviconURL(
    int32 page_id,
    const std::vector<FaviconURL>& candidates)
{
  if(wrapper_) {
    NodeThread::Get()->PostTask(
        FROM_HERE,
        base::Bind(&ExoFrameWrap::DispatchFaviconUpdate, wrapper_, candidates));
  }
}

void 
ExoFrame::DidFailLoad(
    int64 frame_id,
    const GURL& validated_url,
    bool is_main_frame,
    int error_code,
    const string16& error_description,
    RenderViewHost* render_view_host)
{
  if(is_main_frame && wrapper_) {
    std::string desc = UTF16ToUTF8(error_description);
    NodeThread::Get()->PostTask(
        FROM_HERE,
        base::Bind(&ExoFrameWrap::DispatchLoadFail, wrapper_,
                   validated_url.spec(), error_code, desc));
  }
}

void 
ExoFrame::DidFinishLoad(
    int64 frame_id,
    const GURL& validated_url,
    bool is_main_frame,
    RenderViewHost* render_view_host)
{
  if(is_main_frame && wrapper_) {
    NodeThread::Get()->PostTask(
        FROM_HERE,
        base::Bind(&ExoFrameWrap::DispatchLoadFinish, wrapper_,
                   validated_url.spec()));
    web_contents_->GetView()->Focus();
  }
}

void 
ExoFrame::DidStartLoading(
    RenderViewHost* render_view_host)
{
  if(wrapper_) {
    NodeThread::Get()->PostTask(
        FROM_HERE,
        base::Bind(&ExoFrameWrap::DispatchLoadingStart, wrapper_));
  }
}

void 
ExoFrame::DidStopLoading(
    RenderViewHost* render_view_host)
{
  if(wrapper_) {
    NodeThread::Get()->PostTask(
        FROM_HERE,
        base::Bind(&ExoFrameWrap::DispatchLoadingStop, wrapper_));
  }
}


void 
ExoFrame::Observe(
    int type,
    const NotificationSource& source,
    const NotificationDetails& details) 
{
  NOTREACHED();
}


} // namespace exo_browser

