﻿
#include "sceneMgr.h"

#include <ProtoClient.h>
#include <ProtoDocker.h>
#include <ProtoSceneCommon.h>
#include <ProtoSceneServer.h>
#include <ProtoSceneClient.h>

SceneMgr::SceneMgr()
{

}

bool SceneMgr::init(const std::string & configName, ui32 serverID)
{
    if (!ServerConfig::getRef().parseDB(configName) || !ServerConfig::getRef().parseWorld(configName) || !ServerConfig::getRef().parseScenes(configName, serverID))
    {
        LOGE("SceneMgr::init error. parse config error. config path=" << configName);
        return false;
    }

    if (!DBDict::getRef().initHelper())
    {
        LOGE("SceneMgr::init error. DBDict initHelper error. ");
        return false;
    }

    if (!DBDict::getRef().load())
    {
        LOGE("SceneMgr::init error. DBDict load error. ");
        return false;
    }

    _lastSceneID = ServerConfig::getRef().getSceneConfig()._lineID * 10000;
    onTimer();
    return true;
}

bool SceneMgr::loadScenes()
{
    _scenes.clear();
    SceneID lastID = ServerConfig::getRef().getSceneConfig()._lineID * 1000;
    for (int i=0; i<50; i++)
    {
        lastID++;
        auto scene = std::make_shared<Scene>(lastID);
        _scenes.insert(std::make_pair(lastID, scene));
        _frees.push(scene);
    }
    onTimer();
    return true;
}

ScenePtr SceneMgr::getScene(SceneID sceneID)
{
    auto founder = _scenes.find(sceneID);
    if (founder == _scenes.end())
    {
        return nullptr;
    }
    return founder->second;
}
ScenePtr SceneMgr::getActiveScene(SceneID sceneID)
{
    auto founder = _actives.find(sceneID);
    if (founder != _actives.end())
    {
        return founder->second;
    }
    return nullptr;
}
void SceneMgr::onTimer()
{
    if (isStopping())
    {
        return ;
    }
    SessionManager::getRef().createTimer((ui32)(SceneFrameInterval * 1000), std::bind(&SceneMgr::onTimer, this));

    std::list<ScenePtr> frees;
    for (auto kv : _actives)
    {
        try
        {
            bool active = kv.second->onUpdate();
            if (!active)
            {
                frees.push_back(kv.second);
                //send report to world
                SceneSectionNotice notice;
                kv.second->getSceneSection(notice.section);
                //status ins
                std::set<GroupID> groups;
                for (auto &entity : notice.section.entitys)
                {
                    if (entity.state.etype == ENTITY_PLAYER)
                    {
                        groups.insert(entity.state.groupID);
                    }
                }
                for (auto key : groups)
                {
                    SceneServerGroupStateFeedback ret;
                    ret.groupID = key;
                    ret.sceneID = notice.section.sceneID;
                    ret.state = SCENE_STATE_NONE;
                    sendToWorld(ret);
                }

            }
        }
        catch (const std::exception & e)
        {
            LOGE(e.what());
        }
        catch (...)
        {
            LOGE("...");
        }
    }
    for (auto scene : frees)
    {
        _frees.push(scene);
        if (scene->getSceneType() == SCENE_HOME)
        {
            _homes.erase(scene->getSceneID());
        }
        _actives.erase(scene->getSceneID());
        scene->cleanScene();
    }
    return ;
}

void sigInt(int sig)
{
    if (!SceneMgr::getRef().isStopping())
    {
        SessionManager::getRef().post(std::bind(&SceneMgr::stop, SceneMgr::getPtr()));
    }
}

void SceneMgr::forceStop()
{

}

void SceneMgr::stop()
{
    LOGA("SceneMgr::stop");
    SceneMgr::getRef().onShutdown();
}

bool SceneMgr::run()
{
    SessionManager::getRef().run();
    LOGA("SceneMgr::run exit!");
    return true;
}

bool SceneMgr::isStopping()
{
    return false;
}

void SceneMgr::onShutdown()
{
    if (_clientListen != InvalidAccepterID)
    {
        SessionManager::getRef().stopAccept(_clientListen);
        SessionManager::getRef().kickClientSession(_clientListen);
    }
    SessionManager::getRef().kickClientSession();
    SessionManager::getRef().stop();
    return ;
}

bool SceneMgr::startClientListen()
{
    auto sc = ServerConfig::getRef().getSceneConfig();

    _clientListen = SessionManager::getRef().addAccepter(sc._clientListenHost, sc._clientListenPort);
    if (_clientListen == InvalidAccepterID)
    {
        LOGE("SceneMgr::startClientListen addAccepter error. bind ip=" << sc._clientListenHost << ", bind port=" << sc._clientListenPort);
        return false;
    }
    auto &options = SessionManager::getRef().getAccepterOptions(_clientListen);
    options._maxSessions = 1000;
    options._sessionOptions._onSessionPulse = [](TcpSessionPtr session)
    {
        SceneClientPulse pulse;
        WriteStream ws(pulse.getProtoID());
        ws << pulse;
        session->send(ws.getStream(), ws.getStreamLen());
    };
    options._sessionOptions._onSessionLinked = std::bind(&SceneMgr::event_onClientLinked, this, _1);
    options._sessionOptions._onSessionClosed = std::bind(&SceneMgr::event_onClientClosed, this, _1);
    options._sessionOptions._onBlockDispatch = std::bind(&SceneMgr::event_onClientMessage, this, _1, _2, _3);
    if (!SessionManager::getRef().openAccepter(_clientListen))
    {
        LOGE("SceneMgr::startClientListen openAccepter error. bind ip=" << sc._clientListenHost << ", bind port=" << sc._clientListenPort);
        return false;
    }
    LOGA("SceneMgr::startClientListen openAccepter success. bind ip=" << sc._clientListenHost << ", bind port=" << sc._clientListenPort
        <<", _clientListen=" << _clientListen);
    return true;
}



bool SceneMgr::startWorldConnect()
{
    auto wc = ServerConfig::getRef().getWorldConfig();
    
    _worldSessionID = SessionManager::getRef().addConnecter(wc._scenePubHost, wc._sceneListenPort);
    if (_worldSessionID == InvalidSessionID)
    {
        LOGE("SceneMgr::startWorldConnect openConnecter error. remote ip=" << wc._scenePubHost << ", remote port=" << wc._sceneListenPort);
        return false;
    }
    auto &options = SessionManager::getRef().getConnecterOptions(_worldSessionID);
    options._sessionPulseInterval = (unsigned int)(ServerPulseInterval * 1000);
    options._onSessionPulse = [](TcpSessionPtr session)
    {
		if (getFloatSteadyNowTime() - session->getUserParamDouble(UPARAM_LAST_ACTIVE_TIME) > ServerPulseInterval *3.0  )
		{
			LOGE("SceneMgr check session last active timeout. diff=" << getFloatSteadyNowTime() - session->getUserParamDouble(UPARAM_LAST_ACTIVE_TIME));
			session->close();
			return;
		}
		ScenePulse pulse;
        WriteStream ws(pulse.getProtoID());
        ws << pulse;
        session->send(ws.getStream(), ws.getStreamLen());
    };
    options._reconnects = 500;
    options._onSessionLinked = std::bind(&SceneMgr::event_onWorldLinked, this, _1);
    options._onSessionClosed = std::bind(&SceneMgr::event_onWorldClosed, this, _1);
    options._onBlockDispatch = std::bind(&SceneMgr::event_onWorldMessage, this, _1, _2, _3);
    if (!SessionManager::getRef().openConnecter(_worldSessionID))
    {
        LOGE("SceneMgr::startWorldConnect openConnecter error. remote ip=" << wc._scenePubHost << ", remote port=" << wc._sceneListenPort);
        return false;
    }
    LOGA("SceneMgr::startWorldConnect openAccepter success. remote ip=" << wc._scenePubHost
        << ", remote port=" << wc._sceneListenPort <<", openConnecter=" << _worldSessionID);
    return true;
}


void SceneMgr::sendToWorld(const char * block, unsigned int len)
{
    if (_worldSessionID != InvalidSessionID)
    {
        sendViaSessionID(_worldSessionID, block, len);
    }
}
void SceneMgr::sendToWorld(const Tracing &trace, const char * block, unsigned int len)
{
    WriteStream fd(ForwardToService::getProtoID());
    fd << trace;
    fd.appendOriginalData(block, len);
    sendToWorld(fd.getStream(), fd.getStreamLen());
}

void SceneMgr::sendViaSessionID(SessionID sessionID, const char * block, unsigned int len)
{
    SessionManager::getRef().sendSessionData(sessionID, block, len);
}




bool SceneMgr::start()
{
    return startClientListen() && startWorldConnect();
    
}

void SceneMgr::event_onWorldLinked(TcpSessionPtr session)
{
    session->setUserParamDouble(UPARAM_LAST_ACTIVE_TIME, getFloatSteadyNowTime());
    session->setUserParam(UPARAM_AREA_ID, InvalidAreaID);
    session->getOptions()._reconnects = 0;
	SceneKnock notice;
    notice.lineID = ServerConfig::getRef().getSceneConfig()._lineID;
	notice.pubHost = ServerConfig::getRef().getSceneConfig()._clientPubHost;
	notice.pubPort = ServerConfig::getRef().getSceneConfig()._clientListenPort;
	sendViaSessionID(session->getSessionID(), notice);
	LOGI("event_onWorldLinked cID=" << session->getSessionID() );
}


void SceneMgr::event_onWorldClosed(TcpSessionPtr session)
{


}


void SceneMgr::event_onWorldMessage(TcpSessionPtr   session, const char * begin, unsigned int len)
{
    ReadStream rsShell(begin, len);
    if (ScenePulse::getProtoID() != rsShell.getProtoID())
    {
        LOGT("event_onWorldMessage protoID=" << rsShell.getProtoID() << ", len=" << len);
    }

    if (rsShell.getProtoID() == ScenePulse::getProtoID())
    {
        session->setUserParamDouble(UPARAM_LAST_ACTIVE_TIME, getFloatSteadyNowTime());
        return;
    }
    else if (rsShell.getProtoID() == SceneGroupAvatarInfo::getProtoID())
    {
        SceneGroupAvatarInfo info;
        rsShell >> info;
        onSceneGroupAvatarInfo(session, info);
        return;
    }
    else if (rsShell.getProtoID() == SceneServerEnterSceneIns::getProtoID())
    {
        SceneServerEnterSceneIns ins;
        rsShell >> ins;
        onSceneServerEnterSceneIns(session, ins);
        return;
    }
    else if (rsShell.getProtoID() == SceneServerCancelSceneIns::getProtoID())
    {
        SceneServerCancelSceneIns ins;
        rsShell >> ins;
        onSceneServerCancelSceneIns(session, ins);
        return;
    }
    else if (rsShell.getProtoID() == ForwardToService::getProtoID())
    {
        Tracing trace;
        rsShell >> trace;
        ReadStream rs(rsShell.getStreamUnread(), rsShell.getStreamUnreadLen());
        onForwardToService(session, trace, rs);
        return;
    }
    else if (rsShell.getProtoID() == ChatResp::getProtoID())
    {
      
        ChatResp resp;
        rsShell >> resp;
        auto checkToken = _tokens.find(resp.sourceID);
        if (checkToken == _tokens.end())
        {
            LOGE("");
            return;
        }
        auto checkscene = _actives.find(checkToken->second.second);
        if (checkscene == _actives.end())
        {
            LOGE("");
            return;
        }
        auto entity = checkscene->second->getEntityByAvatarID(resp.sourceID);
        if (!entity)
        {
            LOGE("");
            return;
        }
        if (resp.channelID == CC_CAMP)
        {
            auto & players = checkscene->second->getPlayers();
            for (auto player : players)
            {
                if (player.second->_state.camp == entity->_state.camp)
                {
                    resp.targetID = player.second->_state.avatarID;
                    resp.targetName = player.second->_state.avatarName;
                    sendToWorld(resp);
                }
            }
        }
        else if (resp.channelID == CC_SCENE)
        {
            auto & players = checkscene->second->getPlayers();
            for (auto player : players)
            {
                resp.targetID = player.second->_state.avatarID;
                resp.targetName = player.second->_state.avatarName;
                sendToWorld(resp);
            }
        }
        
    }

}







void SceneMgr::event_onClientLinked(TcpSessionPtr session)
{
    LOGD("SceneMgr::event_onClientLinked. SessionID=" << session->getSessionID() 
        << ", remoteIP=" << session->getRemoteIP() << ", remotePort=" << session->getRemotePort());
}

void SceneMgr::event_onClientPulse(TcpSessionPtr session)
{
    auto last = session->getUserParamDouble(UPARAM_LAST_ACTIVE_TIME);
    if (getFloatSteadyNowTime() - last > ClientPulseInterval * 3)
    {
        LOGW("client timeout . diff time=" << getFloatSteadyNowTime() - last << ", sessionID=" << session->getSessionID());
        session->close();
        return;
    }
}
void SceneMgr::event_onClientClosed(TcpSessionPtr session)
{
    LOGD("SceneMgr::event_onClientClosed. SessionID=" << session->getSessionID() 
        << ", remoteIP=" << session->getRemoteIP() << ", remotePort=" << session->getRemotePort());
    if (isConnectID(session->getSessionID()))
    {
        LOGF("Unexpected");
        return;
    }

    if (session->getUserParamNumber(UPARAM_SESSION_STATUS) == SSTATUS_ATTACHED)
    {
        SceneID sceneID = (SceneID)session->getUserParamNumber(UPARAM_SCENE_ID);
        ServiceID avatarID = (ServiceID)session->getUserParamNumber(UPARAM_AVATAR_ID);
        auto scene = getActiveScene(sceneID);
        if (scene)
        {
            scene->playerDettach(avatarID, session->getSessionID());
        }
    }
}



void SceneMgr::event_onClientMessage(TcpSessionPtr session, const char * begin, unsigned int len)
{
    ReadStream rs(begin, len);
    SessionStatus sessionStatus = (SessionStatus)session->getUserParamNumber(UPARAM_SESSION_STATUS);
    ServiceID avatarID = (ServiceID)session->getUserParamNumber(UPARAM_AVATAR_ID);
    SceneID sceneID = (SceneID)session->getUserParamNumber(UPARAM_SCENE_ID);

    if (sessionStatus == SSTATUS_UNKNOW && rs.getProtoID() == AttachSceneReq::getProtoID())
    {
        AttachSceneReq req;
        rs >> req;
        auto founder = _tokens.find(req.avatarID);
        if (founder != _tokens.end() && founder->second.first == req.token  && _actives.find(req.sceneID) != _actives.end())
        {
            session->setUserParam(UPARAM_SESSION_STATUS, SSTATUS_ATTACHED);
            session->setUserParam(UPARAM_AVATAR_ID, req.avatarID);
            session->setUserParam(UPARAM_SCENE_ID, req.sceneID);
            auto scene = _actives.find(req.sceneID)->second;
            sendViaSessionID(session->getSessionID(), AttachSceneResp(EC_SUCCESS, req.avatarID, req.sceneID));
            scene->playerAttach(req.avatarID, session->getSessionID());
        }
        else
        {
            sendViaSessionID(session->getSessionID(), AttachSceneResp(EC_ERROR, req.avatarID, req.sceneID));
        }
    }
    else if (sessionStatus == SSTATUS_ATTACHED)
    {
        if (rs.getProtoID() == SceneClientPulse::getProtoID())
        {
            session->setUserParamDouble(UPARAM_LAST_ACTIVE_TIME, getFloatSteadyNowTime());
            return;
        }
        auto foundScene = _actives.find(sceneID);
        if (foundScene == _actives.end())
        {
            LOGE("");
            return;
        }
        foundScene->second->onPlayerInstruction(avatarID, rs);
    }
    else
    {
        LOGE("client unknow proto or wrong status. protoID=" << rs.getProtoID() << ", status=" << sessionStatus << ", sessionID=" << session->getSessionID());
    }
}



void SceneMgr::onForwardToService(TcpSessionPtr session, Tracing & trace, ReadStream & rs)
{

}
void SceneMgr::onSceneServerCancelSceneIns(TcpSessionPtr session, SceneServerCancelSceneIns & ins)
{
    auto scene = getActiveScene(ins.sceneID);
    if (!scene)
    {
        //
        return;
    }
    scene->removePlayerByGroupID(ins.groupID);
}


void SceneMgr::onSceneGroupAvatarInfo(TcpSessionPtr session, SceneGroupAvatarInfo & info)
{
    auto finder =  _tokens.find(info.avatarID);
    if (finder == _tokens.end())
    {
        LOGE("SceneMgr::onSceneGroupAvatarInfo not found token. avatar=" << info.avatarID << ", name=" << info.avatarName);
        return;
    }
    if (finder->second.second == InvalidSceneID)
    {
        LOGE("SceneMgr::onSceneGroupAvatarInfo scene not invalid. avatar=" << info.avatarID << ", name=" << info.avatarName);
        return;
    }

    auto scene = getActiveScene(finder->second.second);
    if (!scene)
    {
        LOGE("SceneMgr::onSceneGroupAvatarInfo scene not active. avatar=" << info.avatarID << ", name=" << info.avatarName << ", sceneID=" << finder->second.second);
        return;
    }
    auto entity = scene->getEntityByAvatarID(info.avatarID);
    if (!entity)
    {
        LOGE("SceneMgr::onSceneGroupAvatarInfo not found entity. avatar=" << info.avatarID << ", name=" << info.avatarName << ", sceneID=" << finder->second.second);
        return;
    }
    entity->_state.avatarName = info.avatarName;
    entity->_state.modelID = info.modelID;
    entity->_isInfoDirty = true;
}
void SceneMgr::onSceneServerEnterSceneIns(TcpSessionPtr session, SceneServerEnterSceneIns & ins)
{
    ScenePtr scene;
    //如果类型是主城并且存在未满人的主城 直接丢进去
    if (ins.sceneType == SCENE_HOME)
    {
        for (auto &kv : _homes)
        {
            if( kv.second->getPlayerCount() < 30 )
            {
                scene = kv.second;
                break;
            }
        }
    }
   
    if (!scene )
    {
        if (_frees.empty())
        {
            auto scene = std::make_shared<Scene>(++_lastSceneID);
            _scenes.insert(std::make_pair(scene->getSceneID(), scene));
            _frees.push(scene);
        }
        scene = _frees.front();
        _frees.pop();
        scene->initScene((SCENE_TYPE)ins.sceneType, ins.mapID);
        _actives.insert(std::make_pair(scene->getSceneID(), scene));
        if (ins.sceneType == SCENE_HOME)
        {
            _homes.insert(std::make_pair(scene->getSceneID(), scene));
        }
    }
    for (auto & group : ins.groups)
    {
        for (auto & avatar : group.members)
        {
            _tokens[avatar.first] = std::make_pair(avatar.second.token, scene->getSceneID());
            auto entity = scene->makeEntity(avatar.second.modelID,
                    avatar.second.avatarID,
                    avatar.second.avatarName,
                    avatar.second.equips,
                    group.groupID);
                entity->_props.hp = 10000;
                entity->_props.moveSpeed = 6.0;
                entity->_props.attackSpeed = 1.0;
                entity->_props.attack = 200;
                entity->_state.maxHP = entity->_props.hp;
                entity->_state.curHP = entity->_state.maxHP;
                entity->_state.camp = ENTITY_CAMP_BLUE + rand() % 100;
                entity->_skillSys.dictBootSkills[1] = DBDict::getRef().getOneKeyDictSkill(1).second;
                scene->addEntity(entity);
        }
        SceneServerGroupStateFeedback ret;
        ret.groupID = group.groupID;
        ret.sceneID = scene->getSceneID();
        ret.state = SCENE_STATE_WAIT;
        sendToWorld(ret);
        ret.state = SCENE_STATE_ACTIVE;
        sendToWorld(ret);
    }



}





