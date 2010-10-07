/*
 * $Id$
 *
 * Copyright (C) 2002-2003 Fhg Fokus
 *
 * This file is part of sems, a free SIP media server.
 *
 * sems is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * For a license to use the ser software under conditions
 * other than those described here, or to purchase support for this
 * software, please contact iptel.org by e-mail at the following addresses:
 *    info@iptel.org
 *
 * sems is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License 
 * along with this program; if not, write to the Free Software 
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "AmSession.h"
#include "AmSdp.h"
#include "AmConfig.h"
#include "AmUtils.h"
#include "AmPlugIn.h"
#include "AmApi.h"
#include "AmSessionContainer.h"
#include "AmSessionProcessor.h"
#include "AmMediaProcessor.h"
#include "AmDtmfDetector.h"
#include "AmPlayoutBuffer.h"

#ifdef WITH_ZRTP
#include "AmZRTP.h"
#endif

#include "log.h"

#include <algorithm>

#include <unistd.h>
#include <assert.h>
#include <sys/time.h>


volatile unsigned int AmSession::session_num = 0;
AmMutex AmSession::session_num_mut;


// AmSession methods


AmSession::AmSession()
  : AmEventQueue(this),
    dlg(this),
    detached(true),
    sess_stopped(false),
    input(0), output(0), local_input(0), local_output(0),
    m_dtmfDetector(this), m_dtmfEventQueue(&m_dtmfDetector),
    m_dtmfDetectionEnabled(true),
    accept_early_session(false),
    refresh_method(REFRESH_UPDATE_FB_REINV),
    processing_status(SESSION_PROCESSING_EVENTS)
#ifdef WITH_ZRTP
  ,  zrtp_session(NULL), zrtp_audio(NULL), enable_zrtp(true)
#endif

#ifdef SESSION_THREADPOOL
  , _pid(this)
#endif
{
  use_local_audio[AM_AUDIO_IN] = false;
  use_local_audio[AM_AUDIO_OUT] = false;
}

AmSession::~AmSession()
{
  for(vector<AmSessionEventHandler*>::iterator evh = ev_handlers.begin();
      evh != ev_handlers.end(); evh++) {
    
    if((*evh)->destroy)
      delete *evh;
  }

#ifdef WITH_ZRTP
  AmZRTP::freeSession(zrtp_session);
#endif

  DBG("AmSession destructor finished\n");
}

void AmSession::setCallgroup(const string& cg) {
  callgroup = cg;
}

string AmSession::getCallgroup() {
  return callgroup;
}

void AmSession::changeCallgroup(const string& cg) {
  callgroup = cg;
  AmMediaProcessor::instance()->changeCallgroup(this, cg);
}

void AmSession::addHandler(AmSessionEventHandler* sess_evh)
{
  if (sess_evh != NULL)
    ev_handlers.push_back(sess_evh);
}

void AmSession::setInput(AmAudio* in)
{
  lockAudio();
  input = in;
  unlockAudio();
}

void AmSession::setOutput(AmAudio* out)
{
  lockAudio();
  output = out;
  unlockAudio();
}

void AmSession::setInOut(AmAudio* in,AmAudio* out)
{
  lockAudio();
  input = in;
  output = out;
  unlockAudio();
}

void AmSession::setLocalInput(AmAudio* in)
{
  lockAudio();
  local_input = in;
  unlockAudio();
}

void AmSession::setLocalOutput(AmAudio* out)
{
  lockAudio();
  local_output = out;
  unlockAudio();
}

void AmSession::setLocalInOut(AmAudio* in,AmAudio* out)
{
  lockAudio();
  local_input = in;
  local_output = out;
  unlockAudio();
}

void AmSession::setAudioLocal(unsigned int dir, 
			      bool local) {
  assert(dir<2);
  use_local_audio[dir] = local;
}

bool AmSession::getAudioLocal(unsigned int dir) { 
  assert(dir<2); 
  return use_local_audio[dir]; 
}

void AmSession::lockAudio()
{ 
  audio_mut.lock();
}

void AmSession::unlockAudio()
{
  audio_mut.unlock();
}

const string& AmSession::getCallID() const
{ 
  return dlg.callid;
}

const string& AmSession::getRemoteTag() const
{ 
  return dlg.remote_tag;
}

const string& AmSession::getLocalTag() const
{
  return dlg.local_tag;
}

void AmSession::setUri(const string& uri)
{
  DBG("AmSession::setUri(%s)\n",uri.c_str());
  sdp.uri = uri;
}

void AmSession::setLocalTag()
{
  if (dlg.local_tag.empty()) {
    dlg.local_tag = getNewId();
    DBG("AmSession::setLocalTag() - session id set to %s\n", 
	dlg.local_tag.c_str());
  }
}

void AmSession::setLocalTag(const string& tag)
{
  DBG("AmSession::setLocalTag(%s)\n",tag.c_str());
  dlg.local_tag = tag;
}

const vector<SdpPayload*>& AmSession::getPayloads()
{
  return m_payloads;
}

int AmSession::getRPort()
{
  return RTPStream()->getRPort();
}

AmPayloadProviderInterface* AmSession::getPayloadProvider() {
  // by default the system codecs
  return AmPlugIn::instance();
}

// todo: - move this back into AmRtpAudio
//       - simplify payloads handling and move to AmRtpAudio
//         entirely
// AmAudioRtpFormat* AmSession::getNewRtpFormat() {
//   return new AmAudioRtpFormat(m_payloads);
// }

/*
void AmSession::negotiate(const string& sdp_body,
			  bool force_symmetric_rtp,
			  string* sdp_reply)
{
  string r_host = "";
  int    r_port = 0;

  sdp.setBody(sdp_body.c_str());

  if(sdp.parse())
    throw AmSession::Exception(400,"session description parsing failed");

  if(sdp.media.empty())
    throw AmSession::Exception(400,"no media line found in SDP message");
    
  m_payloads = sdp.getCompatiblePayloads(getPayloadProvider(), MT_AUDIO, r_host, r_port);

  if (m_payloads.size() == 0)
    throw AmSession::Exception(488,"could not find compatible payload");
    
  const SdpPayload *telephone_event_payload = sdp.telephoneEventPayload();
  if(telephone_event_payload)
    {
      DBG("remote party supports telephone events (pt=%i)\n",
	  telephone_event_payload->payload_type);
	
      lockAudio();
      RTPStream()->setTelephoneEventPT(telephone_event_payload);
      unlockAudio();
    }
  else {
    DBG("remote party doesn't support telephone events\n");
  }

  bool passive_mode = false;
  if( sdp.remote_active || force_symmetric_rtp) {
    DBG("The other UA is NATed: switched to passive mode.\n");
    DBG("remote_active = %i; force_symmetric_rtp = %i\n",
	sdp.remote_active, force_symmetric_rtp);

    passive_mode = true;
  }

  lockAudio();
  try {
    RTPStream()->setLocalIP(AmConfig::LocalIP);
    RTPStream()->setPassiveMode(passive_mode);
    RTPStream()->setRAddr(r_host, r_port);
  } catch (const string& err_str) {
    unlockAudio();
    throw AmSession::Exception(400, err_str);
  } catch (...) {
    unlockAudio();
    throw;
  }
  unlockAudio();

  if(sdp_reply)
    sdp.genResponse(advertisedIP(), 
		    RTPStream()->getLocalPort(), 
		    *sdp_reply, AmConfig::SingleCodecInOK);
}
*/

#ifdef SESSION_THREADPOOL
void AmSession::start() {
  AmSessionProcessorThread* processor_thread = 
    AmSessionProcessor::getProcessorThread();
  if (NULL == processor_thread) 
    throw string("no processing thread available");

  // have the thread register and start us
  processor_thread->startSession(this);
}

bool AmSession::is_stopped() {
  return processing_status == SESSION_ENDED_DISCONNECTED;
}
#else
// in this case every session has its own thread 
// - this is the main processing loop
void AmSession::run() {
  DBG("startup session\n");
  if (!startup())
    return;

  DBG("running session event loop\n");
  while (true) {
    waitForEvent();
    if (!processingCycle())
      break;
  }

  DBG("session event loop ended, finalizing session\n");
  finalize();
}
#endif

bool AmSession::startup() {
#ifdef WITH_ZRTP
  if (enable_zrtp) {
    zrtp_session = (zrtp_conn_ctx_t*)malloc(sizeof(zrtp_conn_ctx_t));
    if (NULL == zrtp_session)  {
      ERROR("allocating ZRTP session context mem.\n");
    } else {
      zrtp_profile_t profile;
      zrtp_profile_autoload(&profile, &AmZRTP::zrtp_global);		
      profile.active = false;
      profile.allowclear = true;
      profile.autosecure = true; // automatically go into secure mode at the beginning
      
      if (zrtp_status_ok != zrtp_init_session_ctx( zrtp_session,
						   &AmZRTP::zrtp_global,
						   &profile, 
						   AmZRTP::zrtp_instance_zid) ) {
	ERROR("initializing ZRTP session context\n");
	return false;
      }
      
      zrtp_audio = zrtp_attach_stream(zrtp_session, RTPStream()->get_ssrc());
      zrtp_audio->stream_usr_data = this;
      
      if (NULL == zrtp_audio) {
	ERROR("attaching zrtp stream.\n");
	return false;
      }
      
      DBG("initialized ZRTP session context OK\n");
    }
  }
#endif

  session_num_mut.lock();
  session_num++;
  session_num_mut.unlock();

  try {
    try {

      onStart();

    } 
    catch(const AmSession::Exception& e){ throw e; }
    catch(const string& str){
      ERROR("%s\n",str.c_str());
      throw AmSession::Exception(500,"unexpected exception.");
    }
    catch(...){
      throw AmSession::Exception(500,"unexpected exception.");
    }
    
  } catch(const AmSession::Exception& e){
    ERROR("%i %s\n",e.code,e.reason.c_str());
    onBeforeDestroy();
    destroy();
    
    session_num_mut.lock();
    session_num--;
    session_num_mut.unlock();

    return false;
  }

  return true;
}

bool AmSession::processEventsCatchExceptions() {
  try {
    try {	
      processEvents();
    } 
    catch(const AmSession::Exception& e){ throw e; }
    catch(const string& str){
      ERROR("%s\n",str.c_str());
      throw AmSession::Exception(500,"unexpected exception.");
    } 
    catch(...){
      throw AmSession::Exception(500,"unexpected exception.");
    }    
  } catch(const AmSession::Exception& e){
    ERROR("%i %s\n",e.code,e.reason.c_str());
    return false;
  }
  return true;
}

/** one cycle of the event processing loop. 
    this should be called until it returns false. */
bool AmSession::processingCycle() {

  switch (processing_status) {
  case SESSION_PROCESSING_EVENTS: 
    {
      if (!processEventsCatchExceptions())
	return false; // exception occured, stop processing
      
      AmSipDialog::Status dlg_status = dlg.getStatus();
      bool s_stopped = sess_stopped.get();
      
      DBG("%s/%s: %s, %s, %i UACTransPending\n",
	  dlg.callid.c_str(),getLocalTag().c_str(), 
	  dlgStatusStr(dlg_status),
	  s_stopped?"stopped":"running",
	  dlg.getUACTransPending());
      
      // session running?
      if (!s_stopped || (dlg_status == AmSipDialog::Disconnecting))
	return true;
      
      // session stopped?
      if (s_stopped &&
	  (dlg_status == AmSipDialog::Disconnected)) {
	processing_status = SESSION_ENDED_DISCONNECTED;
	return false;
      }
      
      // wait for session's status to be disconnected
      // todo: set some timer to tear down the session anyway,
      //       or react properly on negative reply to BYE (e.g. timeout)
      processing_status = SESSION_WAITING_DISCONNECTED;
      
      if (dlg_status != AmSipDialog::Disconnected) {
	// app did not send BYE - do that for the app
	if (dlg.bye() != 0) {
	  processing_status = SESSION_ENDED_DISCONNECTED;
	  // BYE sending failed - don't wait for dlg status to go disconnected
	  return false;
	}
      }
      
      return true;
      
    } break;
    
  case SESSION_WAITING_DISCONNECTED: {
    // processing events until dialog status is Disconnected 
    
    if (!processEventsCatchExceptions()) {
      processing_status = SESSION_ENDED_DISCONNECTED;
      return false; // exception occured, stop processing
    }
    bool res = dlg.getStatus() != AmSipDialog::Disconnected;
    if (!res)
      processing_status = SESSION_ENDED_DISCONNECTED;
    return res;
  }; break;

  default: {
    ERROR("unknown session processing state\n");
    return false; // stop processing      
  }
  }
}

void AmSession::finalize() {
  DBG("running finalize sequence...\n");
  onBeforeDestroy();
  destroy();
  
  session_num_mut.lock();
  session_num--;
  session_num_mut.unlock();

  DBG("session is stopped.\n");
}
#ifndef SESSION_THREADPOOL
  void AmSession::on_stop() 
#else
  void AmSession::stop()
#endif  
{
  DBG("AmSession::stop()\n");

  if (!getDetached())
    AmMediaProcessor::instance()->clearSession(this);
  else
    clearAudio();
}

void AmSession::setStopped(bool wakeup) {
  sess_stopped.set(true); 
  if (wakeup) 
    AmSessionContainer::instance()->postEvent(getLocalTag(), 
					      new AmEvent(0));
}

void AmSession::destroy()
{
  DBG("AmSession::destroy()\n");
  AmSessionContainer::instance()->destroySession(this);
}

string AmSession::getNewId()
{
  struct timeval t;
  gettimeofday(&t,NULL);

  string id = "";

  id += int2hex(get_random()) + "-";
  id += int2hex(t.tv_sec) + int2hex(t.tv_usec) + "-";
  id += int2hex((unsigned int)((unsigned long)pthread_self()));

  return id;
}

unsigned int AmSession::getSessionNum()
{
  unsigned int res = 0;
  session_num_mut.lock();
  res = session_num;
  session_num_mut.unlock();
  return res;
}


void AmSession::setInbandDetector(Dtmf::InbandDetectorType t)
{ 
  m_dtmfDetector.setInbandDetector(t); 
}

void AmSession::postDtmfEvent(AmDtmfEvent *evt)
{
  if (m_dtmfDetectionEnabled)
    {
      if (dynamic_cast<AmSipDtmfEvent *>(evt) ||
	  dynamic_cast<AmRtpDtmfEvent *>(evt))
        {   
	  // this is a raw event from sip info or rtp
	  m_dtmfEventQueue.postEvent(evt);
        }
      else 
        {
	  // this is an aggregated event, 
	  // post it into our event queue
	  postEvent(evt);
        }
    }
}

void AmSession::processDtmfEvents()
{
  if (m_dtmfDetectionEnabled)
    {
      m_dtmfEventQueue.processEvents();
    }
}

void AmSession::putDtmfAudio(const unsigned char *buf, int size, int user_ts)
{
  m_dtmfEventQueue.putDtmfAudio(buf, size, user_ts);
}

void AmSession::sendDtmf(int event, unsigned int duration_ms) {
  RTPStream()->sendDtmf(event, duration_ms);
}


void AmSession::onDtmf(int event, int duration_msec)
{
  DBG("AmSession::onDtmf(%i,%i)\n",event,duration_msec);
}

void AmSession::clearAudio()
{
  lockAudio();
  if(input){
    input->close();
    input = 0;
  }
  if(output){
    output->close();
    output = 0;
  }
  if(local_input){
    local_input->close();
    local_input = 0;
  }
  if(local_output){
    local_output->close();
    local_output = 0;
  }
  unlockAudio();
  DBG("Audio cleared !!!\n");
  postEvent(new AmAudioEvent(AmAudioEvent::cleared));
}

void AmSession::process(AmEvent* ev)
{
  CALL_EVENT_H(process,ev);

  DBG("AmSession::process\n");

  if (ev->event_id == E_SYSTEM) {
    AmSystemEvent* sys_ev = dynamic_cast<AmSystemEvent*>(ev);
    if(sys_ev){	
      DBG("Session received system Event\n");
      onSystemEvent(sys_ev);
      return;
    }
  }

  AmSipEvent* sip_ev = dynamic_cast<AmSipEvent*>(ev);
  if(sip_ev){
    (*sip_ev)(&dlg);
    return;
  }

  AmAudioEvent* audio_ev = dynamic_cast<AmAudioEvent*>(ev);
  if(audio_ev){
    onAudioEvent(audio_ev);
    return;
  }

  AmDtmfEvent* dtmf_ev = dynamic_cast<AmDtmfEvent*>(ev);
  if (dtmf_ev) {
    DBG("Session received DTMF, event = %d, duration = %d\n", 
	dtmf_ev->event(), dtmf_ev->duration());
    onDtmf(dtmf_ev->event(), dtmf_ev->duration());
    return;
  }

  AmRtpTimeoutEvent* timeout_ev = dynamic_cast<AmRtpTimeoutEvent*>(ev);
  if(timeout_ev){
    onRtpTimeout();
    return;
  }

#ifdef WITH_ZRTP
  AmZRTPEvent* zrtp_ev = dynamic_cast<AmZRTPEvent*>(ev);
  if(zrtp_ev){
    onZRTPEvent((zrtp_event_t)zrtp_ev->event_id, zrtp_ev->stream_ctx);
    return;
  }
#endif
}

void AmSession::onSipRequest(const AmSipRequest& req)
{
  CALL_EVENT_H(onSipRequest,req);

  DBG("onSipRequest: method = %s\n",req.method.c_str());
  if(req.method == "INVITE"){
    onInvite(req);
  }
  else if(req.method == "ACK"){
    return;
  }
  else if( req.method == "BYE" ){
    onBye(req);
  }
  else if( req.method == "CANCEL" ){
    onCancel(req);
  } 
  else if( req.method == "INFO" ){

    if (req.content_type == "application/dtmf-relay") {
      postDtmfEvent(new AmSipDtmfEvent(req.body));
      dlg.reply(req, 200, "OK");
    } else {
      dlg.reply(req, 415, "Unsupported Media Type");
    }
  }
  else {
    dlg.reply(req, 501, "Not implemented");
  }
}

void AmSession::onSipReply(const AmSipReply& reply,
			   AmSipDialog::Status old_dlg_status)
{
  CALL_EVENT_H(onSipReply, reply, old_dlg_status);

  updateRefreshMethod(reply.hdrs);

  if (old_dlg_status != dlg.getStatus())
    DBG("Dialog status changed %s -> %s (stopped=%s) \n", 
	dlgStatusStr(old_dlg_status), 
	dlgStatusStr(dlg.getStatus()),
	sess_stopped.get() ? "true" : "false");
  else 
    DBG("Dialog status stays %s (stopped=%s)\n", 
	dlgStatusStr(old_dlg_status), 
	sess_stopped.get() ? "true" : "false");


  /*  if (negotiate_onreply) {    
    if(old_dlg_status < AmSipDialog::Connected){
      
      switch(dlg.getStatus()){
	
      case AmSipDialog::Connected:
	
	try {
	  RTPStream()->setMonitorRTPTimeout(true);

	  acceptAudio(reply.body,reply.hdrs);

	  if(!getStopped()){
	    
	    onSessionStart(reply);
		  
	    if(input || output || local_input || local_output)
	      AmMediaProcessor::instance()->addSession(this,
						       callgroup); 
	    else { 
	      DBG("no audio input and output set. "
		  "Session will not be attached to MediaProcessor.\n");
	    }
	  }

	}catch(const AmSession::Exception& e){
	  ERROR("could not connect audio!!!\n");
	  ERROR("%i %s\n",e.code,e.reason.c_str());
	  dlg.bye();
	  setStopped();
	  break;
	}
	break;
	
      case AmSipDialog::Trying:
      case AmSipDialog::Proceeding:
      case AmSipDialog::Early:
	
	switch(reply.code){
	  // todo: 180 with body (remote rbt)
	case 180: { 

	  onRinging(reply);

	  RTPStream()->setMonitorRTPTimeout(false);

	  if(input || output || local_input || local_output)
	    AmMediaProcessor::instance()->addSession(this,
						     callgroup); 
	} break;
	case 183: {
	  if (accept_early_session) {
	    try {

	      setMute(true);

	      acceptAudio(reply.body,reply.hdrs);
	    
	      onEarlySessionStart(reply);

	      RTPStream()->setMonitorRTPTimeout(false);
	      
	      // ping the other side to open fw/NAT/symmetric RTP
	      RTPStream()->ping();

	      if(input || output || local_input || local_output)
		AmMediaProcessor::instance()->addSession(this,
							 callgroup); 
	    } catch(const AmSession::Exception& e){
	      ERROR("%i %s\n",e.code,e.reason.c_str());
	    } // exceptions are not critical here
	  }
	} break;
	default:  break;// continue waiting.
	}

      default: break;
      }
    }
  }
  */
}



void AmSession::onInvite2xx(const AmSipReply& reply)
{
  dlg.send_200_ack(reply.cseq);
}

void AmSession::onNoAck(unsigned int cseq)
{
  if (dlg.getStatus() == AmSipDialog::Connected)
    dlg.bye();
  setStopped();
}

void AmSession::onNoPrack(const AmSipRequest &req, const AmSipReply &rpl)
{
  if (dlg.getStatus() < AmSipDialog::Connected)
    setStopped();
}

void AmSession::onAudioEvent(AmAudioEvent* audio_ev)
{
  if (audio_ev->event_id == AmAudioEvent::cleared)
    setStopped();
}

void AmSession::onInvite(const AmSipRequest& req)
{
  dlg.reply(req,200,"OK");
}

void AmSession::onBye(const AmSipRequest& req)
{
  dlg.reply(req,200,"OK");
  setStopped();
}

// void AmSession::onPrack(const AmSipRequest& req, unsigned cnt)
// {
//   DBG("handling #%u PRACK.\n", cnt);
//   dlg.reply(req, 200, "OK");
// }

void AmSession::onCancel(const AmSipRequest& req)
{
  dlg.reply(req,200,"OK");
  // TODO: if dialog is not yet finally replied,
  //       answer the INVITE transaction with 487
}

void AmSession::onSystemEvent(AmSystemEvent* ev) {
  if (ev->sys_event == AmSystemEvent::ServerShutdown) {
    setStopped();
    return;
  }
}

void AmSession::onSendRequest(const string& method, const string& content_type,
			      const string& body, string& hdrs, int flags, unsigned int cseq)
{
  CALL_EVENT_H(onSendRequest,method,content_type,body,hdrs,flags,cseq);
}

void AmSession::onSendReply(const AmSipRequest& req, unsigned int  code, 
			    const string& reason, const string& content_type,
			    const string& body, string& hdrs, int flags)
{
  CALL_EVENT_H(onSendReply,req,code,reason,content_type,body,hdrs,flags);
}

/** Hook called when an SDP offer is required */
bool AmSession::getSdpOffer(AmSdp& offer)
{
  // TODO: move this code to AmRtpStream

  offer.version = 0;
  offer.origin.user = "sems";
  offer.origin.sessId = 1;
  offer.origin.sessV = 1;
  offer.sessionName = "sems";
  offer.conn.network = NT_IN;
  offer.conn.addrType = AT_V4;
  offer.conn.address = advertisedIP();

  // TODO: support multiple media types
  offer.media.push_back(SdpMedia());
  SdpMedia& offer_media = offer.media[0];

  // TODO: move following code to AmRtpStream

  offer_media.type = MT_AUDIO;
  offer_media.port = RTPStream()->getLocalPort();
  offer_media.nports = 0;
  offer_media.transport = TP_RTPAVP;
  offer_media.dir = SdpMedia::DirBoth;

  getPayloadProvider()->getPayloads(offer_media.payloads);

  return true;
}

/** Hook called when an SDP offer is required */
bool AmSession::getSdpAnswer(const AmSdp& offer, AmSdp& answer)
{
  answer.version = 0;
  answer.origin.user = "sems";
  answer.origin.sessId = 1;
  answer.origin.sessV = 1;
  answer.sessionName = "sems";
  answer.conn.network = NT_IN;
  answer.conn.addrType = AT_V4;
  answer.conn.address = advertisedIP();

  // TODO: support multiple media types
  const vector<SdpMedia>::const_iterator m_it = offer.media.begin();

  answer.media.push_back(SdpMedia());
  SdpMedia& answer_media = answer.media[0];
 
  // TODO: move rest of the function to AmRtpStream

  answer_media.type = MT_AUDIO;
  answer_media.port = RTPStream()->getLocalPort();
  answer_media.nports = 0;
  answer_media.transport = TP_RTPAVP;
  answer_media.dir = SdpMedia::DirBoth;

  // Calculate the intersection with the offered set of payloads

  vector<SdpPayload>::const_iterator it = m_it->payloads.begin();
  for(; it!= m_it->payloads.end(); ++it) {
    amci_payload_t* a_pl = NULL;
    if(it->payload_type < DYNAMIC_PAYLOAD_TYPE_START) {
      // try static payloads
      a_pl = getPayloadProvider()->payload(it->payload_type);
    }

    if( a_pl) {
      answer_media.payloads.push_back(SdpPayload(a_pl->payload_id,a_pl->name,a_pl->sample_rate,0));
    }
    else {
      // Try dynamic payloads
      // and give a chance to broken
      // implementation using a static payload number
      // for dynamic ones.

      int int_pt = getPayloadProvider()->
	getDynPayload(it->encoding_name,
		      it->clock_rate,
		      it->encoding_param);
      if(int_pt != -1){
	answer_media.payloads.push_back(SdpPayload(int_pt,
						   it->encoding_name,
						   it->clock_rate,
						   it->encoding_param));
      }
    }
  }

  return true;
}

int AmSession::onSdpCompleted(const AmSdp& local_sdp, const AmSdp& remote_sdp)
{
  lockAudio();
  // TODO: 
  //   - get the right media ID
  //   - check if the stream coresponding to the media ID 
  //     should be created or updated   
  //
  int ret = RTPStream()->init(getPayloadProvider(),0,local_sdp,remote_sdp);
  unlockAudio();
  
  if(ret){
    ERROR("while initializing RTP stream\n");
    return -1;
  }

  if(detached.get() && !getStopped()) {

    if(dlg.getStatus() == AmSipDialog::Connected)
      onSessionStart();
    else if(dlg.getStatus() == AmSipDialog::Early)
      onEarlySessionStart();

    if(input || output || local_input || local_output) {
      AmMediaProcessor::instance()->addSession(this, callgroup);
    }
    else {
      DBG("no audio input and output set. "
	  "Session will not be attached to MediaProcessor.\n");
    }
  }

  return 0;
}

void AmSession::onRtpTimeout()
{
  DBG("stopping Session.\n");
  setStopped();
}

void AmSession::onSessionTimeout() {
  DBG("Session Timer: Timeout, ending session.\n");
  dlg.bye();
  setStopped();
}

void AmSession::updateRefreshMethod(const string& headers) {
  if (refresh_method == REFRESH_UPDATE_FB_REINV) {
    if (key_in_list(getHeader(headers, SIP_HDR_ALLOW),
		    SIP_METH_UPDATE)) {
      DBG("remote allows UPDATE, using UPDATE for session refresh.\n");
      refresh_method = REFRESH_UPDATE;
    }
  }
}

bool AmSession::refresh() {
  if (refresh_method == REFRESH_UPDATE) {
    DBG("Refreshing session with UPDATE\n");
    return sendUpdate("", "", "") == 0;
  } else {

    if (dlg.getUACInvTransPending()) {
      DBG("INVITE transaction pending - not refreshing now\n");
      return false;
    }

    DBG("Refreshing session with re-INVITE\n");
    return sendReinvite(true) == 0;
  }
}

int AmSession::sendUpdate(const string &cont_type, const string &body,
			   const string &hdrs)
{
  return dlg.update(cont_type, body, hdrs);
}

void AmSession::sendPrack(const string &sdp_offer, 
                          const string &rseq_val, 
                          const string &cseq_val)
{
  string hdrs = "RAck: " + rseq_val + " " + cseq_val + "\r\n";

  // TODO: digest an answer based on the sdp_offer

  // TODO: should't cseq&rseq be handled in dialog, entirely?!?!
  if (dlg.prack(/*cont. type*/"", /*body*/"", hdrs) < 0)
    ERROR("failed to send PRACK request in session '%s'.\n",sid4dbg().c_str());
}

string AmSession::sid4dbg()
{
  string dbg;
  dbg = dlg.callid + "/" + dlg.local_tag + "/" + dlg.remote_tag + "/" + 
      int2str(RTPStream()->getLocalPort()) + "/" + 
      RTPStream()->getRHost() + ":" + int2str(RTPStream()->getRPort());
  return dbg;
}

int AmSession::sendReinvite(bool updateSDP, const string& headers) 
{
  // if (updateSDP) {
  //   RTPStream()->setLocalIP(AmConfig::LocalIP);
  //   string sdp_body;
  //   sdp.genResponse(advertisedIP(), RTPStream()->getLocalPort(), sdp_body);
  //   return dlg.reinvite(headers + get_100rel_hdr(reliable_1xx), SIP_APPLICATION_SDP,
  //       sdp_body);
  // } else {
    return dlg.reinvite(headers, "", "");
  // }
}

int AmSession::sendInvite(const string& headers) 
{
  onOutgoingInvite(headers);

  // Set local IP first, so that IP is set when 
  // getLocalPort/setLocalPort may bind.
  RTPStream()->setLocalIP(AmConfig::LocalIP);
  
  // Generate SDP.
  // string sdp_body;
  // sdp.genRequest(advertisedIP(), RTPStream()->getLocalPort(), sdp_body);
  return dlg.invite(headers, 
		    ""/*SIP_APPLICATION_SDP*/, ""/*sdp_body*/);
}

void AmSession::setOnHold(bool hold)
{
  lockAudio();
  bool old_hold = RTPStream()->getOnHold();
  RTPStream()->setOnHold(hold);
  if (hold != old_hold) 
    sendReinvite();
  unlockAudio();
}

// Utility for basic NAT handling: allow the config file to specify the IP
// address to use in SDP bodies 
string AmSession::advertisedIP()
{
  string set_ip = AmConfig::PublicIP; // "public_ip" parameter. 
  DBG("AmConfig::PublicIP is <%s>.\n", set_ip.c_str());
  if (set_ip.empty())
    return AmConfig::LocalIP;           // "listen" parameter.
  return set_ip;
}  
 
#ifdef WITH_ZRTP
void AmSession::onZRTPEvent(zrtp_event_t event, zrtp_stream_ctx_t *stream_ctx) {
  DBG("AmSession::onZRTPEvent \n");
  switch (event)
    {
    case ZRTP_EVENT_IS_SECURE: {
      INFO("ZRTP_EVENT_IS_SECURE \n");
      //         info->is_verified  = ctx->_session_ctx->secrets.verifieds & ZRTP_BIT_RS0;
 
      zrtp_conn_ctx_t *session = stream_ctx->_session_ctx;
 
      if (ZRTP_SAS_BASE32 == session->sas_values.rendering) {
 	DBG("Got SAS value <<<%.4s>>>\n", session->sas_values.str1.buffer);
      } else {
 	DBG("Got SAS values SAS1 '%s' and SAS2 '%s'\n", 
 	    session->sas_values.str1.buffer,
 	    session->sas_values.str2.buffer);
      } 
    } break;
 
    case ZRTP_EVENT_IS_PENDINGCLEAR:
      INFO("ZRTP_EVENT_IS_PENDINGCLEAR\n");
      INFO("other side requested goClear. Going clear.\n\n");
      zrtp_clear_stream(zrtp_audio);
      break;
 
    case ZRTP_EVENT_IS_CLEAR:
      INFO("ZRTP_EVENT_IS_CLEAR\n");
      break;
 
    case ZRTP_EVENT_UNSUPPORTED:
      INFO("ZRTP_EVENT_UNSUPPORTED\n");
      break;
    case ZRTP_EVENT_IS_INITIATINGSECURE:
      INFO("ZRTP_EVENT_IS_INITIATINGSECURE\n");
      break;
    case ZRTP_EVENT_IS_PENDINGSECURE:
      INFO("ZRTP_EVENT_PENDINGSECURE\n");
      break;
    case ZRTP_EVENT_IS_SECURE_DONE:
      INFO("ZRTP_EVENT_IS_SECURE_DONE\n");
      break;
    case ZRTP_EVENT_ERROR:
      INFO("ZRTP_EVENT_ERROR\n");
      break;
    case ZRTP_EVENT_NO_ZRTP:
      INFO("ZRTP_EVENT_NO_ZRTP\n");
      break;
    case ZRTP_EVENT_NO_ZRTP_QUICK:
      INFO("ZRTP_EVENT_NO_ZRTP_QUICK\n");
      break;
 
      // pbx functions
    case ZRTP_EVENT_IS_CLIENT_ENROLLMENT:
      INFO("ZRTP_EVENT_IS_CLIENT_ENROLLMENT\n");
      break;
    case ZRTP_EVENT_NEW_USER_ENROLLED:
      INFO("ZRTP_EVENT_NEW_USER_ENROLLED\n");
      break;
    case ZRTP_EVENT_USER_ALREADY_ENROLLED:
      INFO("ZRTP_EVENT_USER_ALREADY_ENROLLED\n");
      break;
    case ZRTP_EVENT_USER_UNENROLLED:
      INFO("ZRTP_EVENT_USER_UNENROLLED\n");
      break;
    case ZRTP_EVENT_LOCAL_SAS_UPDATED:
      INFO("ZRTP_EVENT_LOCAL_SAS_UPDATED\n");
      break;
    case ZRTP_EVENT_REMOTE_SAS_UPDATED:
      INFO("ZRTP_EVENT_REMOTE_SAS_UPDATED\n");
      break;
 
      // errors
    case ZRTP_EVENT_WRONG_SIGNALING_HASH:
      INFO("ZRTP_EVENT_WRONG_SIGNALING_HASH\n");
      break;
    case ZRTP_EVENT_WRONG_MESSAGE_HMAC:
      INFO("ZRTP_EVENT_WRONG_MESSAGE_HMAC\n");
      break;
     
    default: 
      INFO("unknown ZRTP_EVENT\n");
      break;
    } // end events case
}
 
#endif

/** EMACS **
 * Local variables:
 * mode: c++
 * c-basic-offset: 2
 * End:
 */
