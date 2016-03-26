
#include "WebrtcConductor.h"

#include <utility>
#include <vector>

#include "talk/app/webrtc/mediastreaminterface.h"
#include "talk/app/webrtc/videosourceinterface.h"
#include "talk/media/devices/devicemanager.h"
#include "webrtc/base/common.h"
#include "webrtc/base/logging.h"
#include "webrtc/base/ssladapter.h"
#include "Global/Global.h"
#include "MediaConstraints.h"
#include "CallObjectQXmppWebrtc.h"
#include "WebrtcFilter.h"
#include "WebrtcQtSocketServer.h"
#include <QApplication>

class DummySetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  static DummySetSessionDescriptionObserver* Create() {
    return
        new rtc::RefCountedObject<DummySetSessionDescriptionObserver>();
  }
  virtual void OnSuccess() {
    LOG(INFO) << __FUNCTION__;
  }
  virtual void OnFailure(const std::string& error) {
    LOG(INFO) << __FUNCTION__ << " " << error;
  }

 protected:
  DummySetSessionDescriptionObserver() {}
  ~DummySetSessionDescriptionObserver() {}
};

CWebrtcConductor::CWebrtcConductor()
{
    rtc::LogMessage::LogToDebug(rtc::LS_NONE);
    rtc::InitializeSSL();
    m_pWebrtcFilter = new CWebrtcFilter(this);
    if(m_pWebrtcFilter)
        qApp->installEventFilter(m_pWebrtcFilter);
    m_pWebrtcQtSocketServer = new CWebrtcQtSocketServer(this);
    if(m_pWebrtcQtSocketServer)
      m_pSignalThread = rtc::ThreadManager::Instance()->CurrentThread();
    if(m_pSignalThread)
    {
        m_pSignalThread->set_socketserver(m_pWebrtcQtSocketServer);
    }    
    m_dtls = false;
    m_pCall = NULL;
}

CWebrtcConductor::~CWebrtcConductor()
{
    if(m_pWebrtcFilter)
    {
        qApp->removeEventFilter(m_pWebrtcFilter);
        delete m_pWebrtcFilter;
    }

    if(m_pWebrtcQtSocketServer)
    {
        m_pSignalThread->set_socketserver(NULL);   
        delete m_pWebrtcQtSocketServer;
    }
    
    rtc::CleanupSSL();
    
}

int CWebrtcConductor::SetCallObject(CCallObjectQXmppWebrtc *pCall)
{    
    m_pCall = pCall;
    return 0;
}

int CWebrtcConductor::PeerConnect()
{
    if (peer_connection_.get()) {
      LOG_MODEL_ERROR("WEBRTC",
          "We only support connecting to one peer at a time");
      return -1;
    }
  
    if (InitializePeerConnection()) {
      peer_connection_->CreateOffer(this, NULL);
    } else {
      LOG_MODEL_ERROR("WEBRTC", "Failed to initialize PeerConnection");
    }
    return 0;
}

int CWebrtcConductor::AcceptConnect(std::string szSdp, std::string szType)
{
    if (!peer_connection_.get()) {
      if (!InitializePeerConnection()) {
          LOG_MODEL_ERROR("WEBRTC", "Failed to initialize accept PeerConnection");
          return -1;
      }
    }

    webrtc::SdpParseError error;
    webrtc::SessionDescriptionInterface* session_description(
        webrtc::CreateSessionDescription(szType, szSdp, &error));
    if (!session_description) {
      LOG_MODEL_WARNING("WEBRTC", "Can't parse received session description message. "
          "SdpParseError was: %s", error.description.c_str());
      return -2;
    }

    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create(), session_description);
    if (session_description->type() ==
        webrtc::SessionDescriptionInterface::kOffer) {
      peer_connection_->CreateAnswer(this, NULL);
    }

    return 0;
}

int CWebrtcConductor::ReciveIceCandidate(std::string szMid, int nIndex, std::string szSdp)
{
    webrtc::SdpParseError error;
    rtc::scoped_ptr<webrtc::IceCandidateInterface> candidate(
                webrtc::CreateIceCandidate(szMid, nIndex, szSdp, &error));
    if (!candidate.get()) {
        LOG_MODEL_WARNING("WEBRTC", "Can't parse received candidate message. SdpParseError was: %s",
                          error.description.c_str());
        return -1;
    }
    if (!peer_connection_->AddIceCandidate(candidate.get())) {
        LOG_MODEL_WARNING("WEBRTC", "Failed to apply the received candidate");
        return -2;
    }
    return 0;
}

int CWebrtcConductor::PeerStop()
{
    int nRet = 0;
    DeletePeerConnection();
    return nRet;
}

bool CWebrtcConductor::InitializePeerConnection()
{
  ASSERT(peer_connection_factory_.get() == NULL);
  ASSERT(peer_connection_.get() == NULL);
  if(peer_connection_.get())
      return false;

  peer_connection_factory_  = webrtc::CreatePeerConnectionFactory();
  if (!peer_connection_factory_.get()) {
    LOG_MODEL_ERROR("WEBRTC",
        "Failed to initialize PeerConnectionFactory");
    DeletePeerConnection();
    return false;
  }

  if (!CreatePeerConnection()) {
    LOG_MODEL_ERROR("WEBRTC",
        "CreatePeerConnection failed");
    DeletePeerConnection();
  }
  AddStreams();
  return peer_connection_.get() != NULL;
}

void CWebrtcConductor::DeletePeerConnection() {
  peer_connection_ = NULL;
  peer_connection_factory_ = NULL;
  m_LocaleVideoRender.release();
  m_RemoteVideoRender.release();
}

bool CWebrtcConductor::CreatePeerConnection() {
  ASSERT(peer_connection_factory_.get() != NULL);
  ASSERT(peer_connection_.get() == NULL);

  webrtc::PeerConnectionInterface::IceServers servers;
  webrtc::PeerConnectionInterface::IceServer server;
  //stun服务器地址  
  QString szStun = "stun:" + CGlobal::Instance()->GetStunServer()
          + ":" + QString::number(CGlobal::Instance()->GetStunServerPort());
  server.uri = szStun.toStdString();
  servers.push_back(server);
  //turn服务器地址  
  QString szTurn = "turn:" + CGlobal::Instance()->GetTurnServer()
          + ":" + QString::number(CGlobal::Instance()->GetTurnServerPort());
  server.uri = szTurn.toStdString();
  server.username = CGlobal::Instance()->GetTurnServerUser().toStdString();
  server.password = CGlobal::Instance()->GetTurnServerPassword().toStdString();
  servers.push_back(server);
  
  CMediaConstraints constraints;
  if (m_dtls) {
    constraints.AddOptional(webrtc::MediaConstraintsInterface::kEnableDtlsSrtp,
                            "true");
  }
  else
  {
    constraints.AddOptional(webrtc::MediaConstraintsInterface::kEnableDtlsSrtp,
                            "false");
  }

  peer_connection_ =
      peer_connection_factory_->CreatePeerConnection(servers,
                                                     &constraints,
                                                     NULL,
                                                     NULL,
                                                     this);
  return peer_connection_.get() != NULL;
}

void CWebrtcConductor::AddStreams()
{
    //建立本地流  
    rtc::scoped_refptr<webrtc::MediaStreamInterface> stream =
        peer_connection_factory_->CreateLocalMediaStream("steam");
    
    //*音频track  
    rtc::scoped_refptr<webrtc::AudioSourceInterface> AudioSource
            = peer_connection_factory_->CreateAudioSource(NULL);
    if(AudioSource.get())
    {
        rtc::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
                    peer_connection_factory_->CreateAudioTrack(
                        "audio_track", AudioSource));
        if(audio_track.get())
        {
            if(!stream->AddTrack(audio_track))
                LOG_MODEL_ERROR("WEBRTC", "Add audio track fail");
        }
        else
            LOG_MODEL_ERROR("WEBRTC", "audio track is null");
    } else{
        LOG_MODEL_WARNING("WEBRTC", "Don't open audio source");
    }//*/
    //视频track  
    if(m_pCall->IsVideo())
    {
        cricket::VideoCapturer* pVC = OpenVideoCaptureDevice();
        if(pVC)
        {
            rtc::scoped_refptr<webrtc::VideoTrackInterface> video_track(
                        peer_connection_factory_->CreateVideoTrack(
                            "video_track",
                            peer_connection_factory_->CreateVideoSource(pVC,
                                                                        NULL)));
            if(video_track.get())
            {
                //关联视频track到本地 Renderer  
                if(!m_pCall->IsMonitor())
                {
                    m_LocaleVideoRender.reset(new CVideoRenderer(video_track, m_pCall, true));
                }
                if(!stream->AddTrack(video_track))
                    LOG_MODEL_ERROR("WEBRTC", "Add video track fail");
            } else {
                LOG_MODEL_ERROR("WEBRTC", "video track is null");
            }
        } else{
            LOG_MODEL_INFO("WEBRTC", "Don't open video source");
        }
    }
    
    if (!peer_connection_->AddStream(stream)) {
      LOG_MODEL_ERROR("WEBRTC", "Adding stream to PeerConnection failed");
    }
}

void CWebrtcConductor::OnAddStream(webrtc::MediaStreamInterface *stream)
{
    //关联视频track到远程 Renderer  
    webrtc::VideoTrackVector tracks = stream->GetVideoTracks();
    // Only render the first track.  
    if (!tracks.empty() && !m_pCall->IsMonitor()) {
        webrtc::VideoTrackInterface* track = tracks[0];
        m_RemoteVideoRender.reset(new CVideoRenderer(track, m_pCall, false));
    }
    m_pCall->slotChanageState(CCallObject::ActiveState);
}

void CWebrtcConductor::OnRemoveStream(webrtc::MediaStreamInterface *stream)
{
}

void CWebrtcConductor::OnIceCandidate(const webrtc::IceCandidateInterface *candidate)
{
    LOG_MODEL_DEBUG("WEBRTC", "sdp_mline_index:%d", candidate->sdp_mline_index());
    std::string sdp;
    if (!candidate->ToString(&sdp)) {
        LOG_MODEL_ERROR("WEBRTC", "Failed to serialize candidate");
        return;
    }
    m_pCall->SendTransportInfo(candidate->sdp_mid().c_str(),
                               candidate->sdp_mline_index(),
                               sdp.c_str());
}

void CWebrtcConductor::OnSuccess(webrtc::SessionDescriptionInterface *desc)
{
    peer_connection_->SetLocalDescription(
         DummySetSessionDescriptionObserver::Create(), desc);
   
    std::string sdp;
    desc->ToString(&sdp);
    
    //通过信令通道发送到对等端  
    m_pCall->SendSessionDescription(sdp.c_str(), desc->type().c_str(), true);
}

void CWebrtcConductor::OnFailure(const std::string &error)
{
    m_pCall->SendSessionDescription(error.c_str(), "", false);
}

cricket::VideoCapturer* CWebrtcConductor::OpenVideoCaptureDevice() {
  rtc::scoped_ptr<cricket::DeviceManagerInterface> dev_manager(
      cricket::DeviceManagerFactory::Create());
  if (!dev_manager->Init()) {
    LOG_MODEL_ERROR("CWebrtcConductor", "Can't create device manager");
    return NULL;
  }
  std::vector<cricket::Device> devs;
  if (!dev_manager->GetVideoCaptureDevices(&devs)) {
    LOG_MODEL_ERROR("CWebrtcConductor", "Can't enumerate video devices");
    return NULL;
  }

  cricket::VideoCapturer* capturer = NULL;
  int nIndex = CGlobal::Instance()->GetVideoCaptureDevice();
  if(-1 < nIndex && nIndex < devs.size())
      capturer = dev_manager->CreateVideoCapturer(devs[nIndex]);
    
  return capturer;
}
