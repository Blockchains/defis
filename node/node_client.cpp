// Copyright 2018 The Beam Team / Copyright 2019 The Grimm Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "node_client.h"

#include <mutex>

#include "pow/external_pow.h"
#include "utility/logger.h"

#include <boost/filesystem.hpp>
#ifdef  XGM_USE_GPU
#include "utility/gpu/gpu_tools.h"
#endif //  XGM_USE_GPU

namespace
{
    constexpr int kVerificationThreadsMaxAvailable = -1;
}

namespace grimm
{
NodeClient::NodeClient(INodeClientObserver* observer)
    : m_observer(observer)
    , m_shouldStartNode(false)
    , m_shouldTerminateModel(false)
    , m_isRunning(false)
{
}

NodeClient::~NodeClient()
{
    try
    {
        m_shouldTerminateModel = true;
        m_waiting.notify_all();
        {
            auto r = m_reactor.lock();
            if (r)
            {
                r->stop();
                if (m_thread)
                {
                    // TODO: check this
                    m_thread->join();
                }
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_UNHANDLED_EXCEPTION() << "what = " << e.what();
    }
    catch (...) {
        LOG_UNHANDLED_EXCEPTION();
    }
}

void NodeClient::setKdf(grimm::Key::IKdf::Ptr kdf)
{
    m_pKdf = kdf;
}

void NodeClient::setOwnerKey(grimm::Key::IPKdf::Ptr key)
{
    m_ownerKey = key;
}

void NodeClient::startNode()
{
    m_shouldStartNode = true;
    m_waiting.notify_all();
}

void NodeClient::stopNode()
{
    m_shouldStartNode = false;
    auto reactor = m_reactor.lock();
    if (reactor)
    {
        reactor->stop();
    }
}

void NodeClient::start()
{
    m_thread = std::make_shared<std::thread>([this]()
    {
        try
        {
            auto reactor = io::Reactor::create();
            m_reactor = reactor;// store weak ref
            io::Reactor::Scope scope(*reactor);

            std::mutex localMutex;

            while (!m_shouldTerminateModel)
            {
                if (!m_shouldStartNode)
                {
                    std::unique_lock<std::mutex> lock(localMutex);

                    while (!m_shouldStartNode && !m_shouldTerminateModel)
                    {
                        m_waiting.wait(lock);
                    }
                }

                if (!m_shouldTerminateModel)
                {
                    bool bErr = true;
                    try
                    {
                        m_shouldStartNode = false;
                        runLocalNode();
                        bErr = false;
                    }
                    catch (const io::Exception& ex)
                    {
                        LOG_ERROR() << ex.what();
                        m_observer->onFailedToStartNode(ex.errorCode);
                        bErr = false;
                    }
                    catch (const std::runtime_error& ex)
                    {
                        LOG_ERROR() << ex.what();
                    }
                    catch (const CorruptionException& ex)
                    {
                        LOG_ERROR() << "Corruption: " << ex.m_sErr;
                    }

                    if (bErr)
                        m_observer->onSyncError(Node::IObserver::Error::Unknown);
                }
            }
        }
        catch (const std::exception& e)
        {
            LOG_UNHANDLED_EXCEPTION() << "what = " << e.what();
        }
        catch (...) {
            LOG_UNHANDLED_EXCEPTION();
        }

        m_observer->onNodeThreadFinished();
    });
}

bool NodeClient::isNodeRunning() const
{
    return m_isRunning;
}

void NodeClient::runLocalNode()
{
  #ifdef XGM_USE_GPU
    std::unique_ptr<IExternalPOW> stratumServer = m_observer->getStratumServer();
    #endif //  XGM_USE_GPU

    Node node;
    node.m_Cfg.m_Listen.port(m_observer->getLocalNodePort());
    node.m_Cfg.m_Listen.ip(INADDR_ANY);
    node.m_Cfg.m_sPathLocal = m_observer->getLocalNodeStorage();
    node.m_Cfg.m_MiningThreads = m_observer->getLocalNodeMiningThreads();

    node.m_Cfg.m_cac_premine = m_observer->getcac_premine();
    node.m_Cfg.m_cac_blockreward = m_observer->getcac_blockreward();
    node.m_Cfg.m_cac_drop0 = m_observer->getcac_drop0();
    node.m_Cfg.m_cac_drop1 = m_observer->getcac_drop1();
    node.m_Cfg.m_cac_coinbasematurity = m_observer->getcac_coinbasematurity();
    node.m_Cfg.m_cac_standartmaturity = m_observer->getcac_standartmaturity();
    node.m_Cfg.m_cac_blocktime = m_observer->getcac_blocktime();
    node.m_Cfg.m_cac_diff = m_observer->getcac_diff();

    node.m_Cfg.m_cac_symbol = m_observer->getLocalNodecac_symbol();
    node.m_Cfg.m_cac_id1 = m_observer->getLocalNodecac_id1();
    node.m_Cfg.m_cac_id2 = m_observer->getLocalNodecac_id2();


    node.m_Cfg.m_VerificationThreads = kVerificationThreadsMaxAvailable;


    Rules::get().isAssetchain = ( node.m_Cfg.m_cac_symbol[0] ) && ( node.m_Cfg.m_cac_symbol != "XGM" ) && ( node.m_Cfg.m_cac_symbol != "GRIMM" ) ;




    LOG_INFO() << "Rules signature: " << Rules::get().get_SignatureStr();

    LOG_INFO() << "start on " << node.m_Cfg.m_Listen.port() << " port..." << " Assetchain Ticker: " << node.m_Cfg.m_cac_symbol << " Assetchain enabled: " << Rules::get().isAssetchain;

    if(m_ownerKey)
    {
        node.m_Keys.m_pOwner = m_ownerKey;
    }
    else
    {
        node.m_Keys.SetSingleKey(m_pKdf);
    }

	node.m_Cfg.m_Horizon.SetStdFastSync();

    auto peers = m_observer->getLocalNodePeers();

    for (const auto& peer : peers)
    {
        io::Address peer_addr;
        if (peer_addr.resolve(peer.c_str()))
        {
            node.m_Cfg.m_Connect.emplace_back(peer_addr);
        }
        else
        {
            LOG_ERROR() << "Unable to resolve node address: " << peer;
        }
    }

    LOG_INFO() << "starting a node on " << node.m_Cfg.m_Listen.port() << " port...";

    struct MyObserver
        :public Node::IObserver
    {
        Node* m_pNode;
        NodeClient* m_pModel;

		bool m_bReportedStarted = false;

        void OnSyncProgress() override
        {
            Node::SyncStatus s = m_pNode->m_SyncStatus;

            if (MaxHeight == m_Done0)
				      m_Done0 = s.m_Done;
			    	s.ToRelative(m_Done0);

			if (!m_bReportedStarted && (s.m_Done == s.m_Total))
			{
				m_bReportedStarted = true;
				m_pModel->m_observer->onStartedNode();
			}

			// make sure no overflow during conversion from SyncStatus to int,int.
			unsigned int nThreshold = static_cast<unsigned int>(std::numeric_limits<int>::max());
            while (s.m_Total > nThreshold)
            {
                s.m_Total >>= 1;
                s.m_Done >>= 1;
            }

            m_pModel->m_observer->onSyncProgressUpdated(static_cast<int>(s.m_Done), static_cast<int>(s.m_Total));
        }

        void OnSyncError(Node::IObserver::Error error) override
        {
            m_pModel->m_observer->onSyncError(error);
        }

		~MyObserver()
		{
			if (m_bReportedStarted)
				m_pModel->m_observer->onStoppedNode();
		}

    Height m_Done0 = MaxHeight;

    } obs;

    obs.m_pNode = &node;
    obs.m_pModel = this;

    node.m_Cfg.m_Observer = &obs;
    #ifdef XGM_USE_GPU
        node.Initialize(stratumServer.get());
        #else
    node.Initialize();
#endif //  XGM_USE_GPU

    if (node.get_AcessiblePeerCount() == 0)
    {
        throw std::runtime_error("Resolved peer list is empty");
    }
    m_isRunning = true;

    io::Reactor::get_Current().run();

    m_isRunning = false;
}
}
