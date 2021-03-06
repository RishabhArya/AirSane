/*
AirSane Imaging Daemon
Copyright (C) 2018-2020 Simul Piscator

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mdnspublisher.h"

#include <avahi-common/thread-watch.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/alternative.h>
#include <avahi-client/publish.h>
#include <avahi-client/client.h>

#include <list>
#include <algorithm>
#include <iostream>
#include <cerrno>
#include <cstring>

namespace {

class AvahiThreadedPollGuard
{
public:
    explicit AvahiThreadedPollGuard(AvahiThreadedPoll* pThread)
      : mpThread(pThread)
    {
        ::avahi_threaded_poll_lock(mpThread);
    }
    ~AvahiThreadedPollGuard()
    {
        ::avahi_threaded_poll_unlock(mpThread);
    }
private:
    AvahiThreadedPoll* mpThread;
};

struct ServiceEntry
{
    MdnsPublisher::Service* mpService;
    AvahiEntryGroup* mpEntryGroup;

    ServiceEntry(MdnsPublisher::Service* p)
        : mpService(p), mpEntryGroup(nullptr)
    {
    }

    ~ServiceEntry()
    {
        unannounce();
    }

    bool announce(AvahiClient* pClient)
    {
        unannounce();
        int err;
        do {
            err = doAnnounce(pClient);
            if(err == AVAHI_ERR_COLLISION)
                renameService();
        } while(err == AVAHI_ERR_COLLISION);
        if(err)
            std::cerr << ::avahi_strerror(err) << std::endl;
        return !err;
    }

    int doAnnounce(AvahiClient* pClient)
    {
        if(mpEntryGroup)
            ::avahi_entry_group_free(mpEntryGroup);
        mpEntryGroup = ::avahi_entry_group_new(pClient, &entryGroupCallback, this);
        if(!mpEntryGroup)
            return ::avahi_client_errno(pClient);
        int err = ::avahi_entry_group_add_service(mpEntryGroup,
              mpService->interfaceIndex(), AVAHI_PROTO_UNSPEC, AvahiPublishFlags(0),
              mpService->name().c_str(), mpService->type().c_str(),
              nullptr, nullptr, mpService->port(), nullptr
        );
        if(!err)
            err = ::avahi_entry_group_commit(mpEntryGroup);
        if(!err) {
            AvahiStringList* txt = nullptr;
            for(const auto& entry : mpService->txtRecord())
                txt = ::avahi_string_list_add_pair(txt, entry.first.c_str(), entry.second.c_str());
            err = ::avahi_entry_group_update_service_txt_strlst(mpEntryGroup,
                  mpService->interfaceIndex(), AVAHI_PROTO_UNSPEC, AvahiPublishFlags(0),
                  mpService->name().c_str(), mpService->type().c_str(),
                  nullptr, txt);
            ::avahi_string_list_free(txt);
        }
        return err;
    }

    void unannounce()
    {
        if(mpEntryGroup) {
            ::avahi_entry_group_free(mpEntryGroup);
            mpEntryGroup = nullptr;
        }
    }

    void renameService()
    {
        char* altname = ::avahi_alternative_service_name(mpService->name().c_str());
        mpService->setName(altname);
        ::avahi_free(altname);
    }

    void onCollision()
    {
        renameService();
        announce(::avahi_entry_group_get_client(mpEntryGroup));
    }

    void onError()
    {
        unannounce();
    }

    static void entryGroupCallback(AvahiEntryGroup*, AvahiEntryGroupState state, void* instance)
    {
        auto p = static_cast<ServiceEntry*>(instance);
        switch(state) {
        case AVAHI_ENTRY_GROUP_COLLISION:
            p->onCollision();
            break;
        case AVAHI_ENTRY_GROUP_FAILURE:
            p->onError();
            break;
        case AVAHI_ENTRY_GROUP_UNCOMMITED:
        case AVAHI_ENTRY_GROUP_REGISTERING:
        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            break;
        }
    }
};

} // namespace

struct MdnsPublisher::Private
{
    AvahiThreadedPoll* mpThread;
    AvahiClient* mpClient;
    AvahiClientState mState;
    std::string mHostNameFqdn;

    std::list<ServiceEntry> mServices;

    Private() : mpThread(nullptr), mpClient(nullptr), mState(AVAHI_CLIENT_CONNECTING)
    {
        mpThread = ::avahi_threaded_poll_new();
        if(mpThread) {
            createClient();
            if(::avahi_threaded_poll_start(mpThread)) {
              int err = errno;
              std::cerr << ::strerror(err) << std::endl;
            }
        }
   }
    ~Private()
    {
        if(mpThread) {
            ::avahi_threaded_poll_stop(mpThread);
            destroyClient();
            ::avahi_threaded_poll_free(mpThread);
        }
    }

    static void clientCallback(AvahiClient* client, AvahiClientState state, void* instance)
    {
        auto p = static_cast<Private*>(instance);
        if(p->mState == AVAHI_CLIENT_CONNECTING) switch(state) {
        case AVAHI_CLIENT_S_COLLISION:
        case AVAHI_CLIENT_S_REGISTERING:
        case AVAHI_CLIENT_S_RUNNING:
            p->onConnected();
            break;
        case AVAHI_CLIENT_FAILURE:
        case AVAHI_CLIENT_CONNECTING:
            break;
        }
        if(state == AVAHI_CLIENT_FAILURE) {
            if(::avahi_client_errno(client) == AVAHI_ERR_DISCONNECTED)
                p->onDisconnected();
            else
                p->onError(::avahi_client_errno(client));
        }
        p->mState = state;
    }

    void createClient()
    {
        destroyClient();
        const AvahiPoll* pPoll = ::avahi_threaded_poll_get(mpThread);
        mpClient = ::avahi_client_new(pPoll, AVAHI_CLIENT_NO_FAIL, &clientCallback, this, nullptr);
        mHostNameFqdn = ::avahi_client_get_host_name_fqdn(mpClient);
    }

    void destroyClient()
    {
        if(mpClient) {
            for(auto& entry : mServices)
                entry.unannounce();
            ::avahi_client_free(mpClient);
        }
        mpClient = nullptr;
        mHostNameFqdn.clear();
    }

    void onConnected()
    {
        for(auto& entry : mServices)
            entry.announce(mpClient);
    }

    void onDisconnected()
    {
        destroyClient();
        createClient();
    }

    void onError(int err)
    {
        std::clog << ::avahi_strerror(err) << std::endl;
        destroyClient();
    }

    std::list<ServiceEntry>::iterator findService(const Service* pService)
    {
        auto i = std::find_if(mServices.begin(), mServices.end(),
            [pService](const ServiceEntry& entry) {
                return entry.mpService == pService;
             });
        return i;
    }

};

MdnsPublisher::MdnsPublisher()
    : p(new Private)
{
}

MdnsPublisher::~MdnsPublisher()
{
    delete p;
}

const std::string& MdnsPublisher::hostNameFqdn() const
{
    return p->mHostNameFqdn;
}

bool MdnsPublisher::announce(MdnsPublisher::Service *pService)
{
    AvahiThreadedPollGuard guard(p->mpThread);
    bool ok = false;
    auto i = p->findService(pService);
    if(i != p->mServices.end()) {
        ok = true;
    } else {
        p->mServices.push_back(ServiceEntry(pService));
        ok = p->mServices.back().announce(p->mpClient);
    }
    return ok;
}

bool MdnsPublisher::unannounce(MdnsPublisher::Service *pService)
{
    AvahiThreadedPollGuard guard(p->mpThread);
    bool ok = false;
    auto i = p->findService(pService);
    if(i != p->mServices.end()) {
        ok = true;
        i->unannounce();
        p->mServices.erase(i);
    }
    return ok;
}

MdnsPublisher::Service &MdnsPublisher::Service::setName(const std::string &s)
{
    std::lock_guard<std::mutex> lock(mNameMutex);
    mName = s;
    return *this;
}

std::string MdnsPublisher::Service::name() const
{
    std::lock_guard<std::mutex> lock(mNameMutex);
    return mName;
}

MdnsPublisher::Service &MdnsPublisher::Service::setTxt(const std::string &key, const std::string &value)
{
    if(!key.empty()) {
        auto i = std::find_if(mTxtRecord.begin(), mTxtRecord.end(),
                              [key](const TxtRecord::value_type& v)
                              {return v.first == key;});
        if(i == mTxtRecord.end())
            mTxtRecord.push_back(std::make_pair(key, value));
        else
            i->second = value;
    }
    return *this;
}
