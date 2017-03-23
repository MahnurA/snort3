//--------------------------------------------------------------------------
// Copyright (C) 2014-2016 Cisco and/or its affiliates. All rights reserved.
// Copyright (C) 2005-2013 Sourcefire, Inc.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// client_app_bit.cc author Sourcefire Inc.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "client_discovery.h"

#include <map>

#include "app_info_table.h"
#include "appid_session.h"
#include "thirdparty_appid_utils.h"
#include "client_app_aim.h"
#include "client_app_bit_tracker.h"
#include "client_app_bit.h"
#include "client_app_msn.h"
#include "client_app_rtp.h"
#include "client_app_ssh.h"
#include "client_app_timbuktu.h"
#include "client_app_tns.h"
#include "client_app_vnc.h"
#include "detector_plugins/detector_http.h"
#include "detector_plugins/detector_imap.h"
#include "detector_plugins/detector_kerberos.h"
#include "detector_plugins/detector_pattern.h"
#include "detector_plugins/detector_pop3.h"
#include "detector_plugins/detector_sip.h"
#include "detector_plugins/detector_smtp.h"
#include "protocols/packet.h"
#include "profiler/profiler.h"

#define MAX_CANDIDATE_CLIENTS 10

ProfileStats clientMatchPerfStats;
THREAD_LOCAL ClientAppMatch* match_free_list = nullptr;

ClientDiscovery::ClientDiscovery()
{
    initialize();
}

ClientDiscovery::~ClientDiscovery()
{
    ClientAppMatch* match;
    while ((match = match_free_list) != nullptr)
    {
        match_free_list = match->next;
        snort_free(match);
    }
}

ClientDiscovery& ClientDiscovery::get_instance()
{
    static THREAD_LOCAL ClientDiscovery* discovery_manager = new ClientDiscovery;
    return *discovery_manager;
}

void ClientDiscovery::initialize()
{
    new AimClientDetector(this);
    new BitClientDetector(this);
    new BitTrackerClientDetector(this);
    new HttpClientDetector(this);
    new ImapClientDetector(this);
    new KerberosClientDetector(this);
    new MsnClientDetector(this);
    new PatternClientDetector(this);
    new Pop3ClientDetector(this);
    new RtpClientDetector(this);
    new SipTcpClientDetector(this);
    new SipUdpClientDetector(this);
    new SmtpClientDetector(this);
    new SshClientDetector(this);
    new TimbuktuClientDetector(this);
    new TnsClientDetector(this);
    new VncClientDetector(this);

    for ( auto kv : tcp_detectors )
        kv.second->initialize();

    for ( auto kv : udp_detectors )
        kv.second->initialize();
}

void ClientDiscovery::finalize_client_plugins()
{
    if ( tcp_patterns )
        tcp_patterns->prep();

    if ( udp_patterns )
        udp_patterns->prep();
}

/*
 * Callback function for string search
 *
 * @param   id      id in array of search strings from pop_config.cmds
 * @param   index   index in array of search strings from pop_config.cmds
 * @param   data    buffer passed in to search function
 *
 * @return response
 * @retval 1        commands caller to stop searching
 */
static int pattern_match(void* id, void* /*unused_tree*/, int match_end_pos, void* data,
    void* /*unused_neg*/)
{
    ClientAppMatch** matches = (ClientAppMatch**)data;
    AppIdPatternMatchNode* pd = (AppIdPatternMatchNode*)id;
    ClientAppMatch* cam;

    // Ignore matches that don't start at the expected position.
    if ( pd->pattern_start_pos >= 0 && pd->pattern_start_pos != (match_end_pos - (int)pd->size))
        return 0;

    for (cam = *matches; cam; cam = cam->next)
    {
        if (cam->detector == pd->service)
            break;
    }

    if (cam)
        cam->count++;
    else
    {
        if (match_free_list)
        {
            cam = match_free_list;
            match_free_list = cam->next;
            memset(cam, 0, sizeof(*cam));
        }
        else
            cam = (ClientAppMatch*)snort_calloc(sizeof(ClientAppMatch));

        cam->count = 1;
        cam->detector =  static_cast<const ClientDetector*>(pd->service);
        cam->next = *matches;
        *matches = cam;
    }

    return 0;
}

static ClientAppMatch* find_detector_candidates(const Packet* pkt, IpProtocol protocol)
{
    ClientAppMatch* match_list = nullptr;
    SearchTool* patterns;

    if (protocol == IpProtocol::TCP)
        patterns = ClientDiscovery::get_instance().tcp_patterns;
    else
        patterns = ClientDiscovery::get_instance().udp_patterns;

    if (!patterns)
        return nullptr;

    patterns->find_all((char*)pkt->data, pkt->dsize, &pattern_match, false, (void*)&match_list);
    return match_list;
}

static const ClientDetector* get_next_detector(ClientAppMatch** match_list)
{
    ClientAppMatch* curr = nullptr;
    ClientAppMatch* prev = nullptr;
    ClientAppMatch* max_curr = nullptr;
    ClientAppMatch* max_prev = nullptr;
    unsigned max_count;
    unsigned max_precedence;

    curr = *match_list;
    max_count = 0;
    max_precedence = 0;
    while (curr)
    {
        if (curr->count >= curr->detector->minimum_matches
            && ((curr->count > max_count)
            || (curr->count == max_count && curr->detector->precedence > max_precedence)))
        {
            max_count = curr->count;
            max_precedence = curr->detector->precedence;
            max_curr = curr;
            max_prev = prev;
        }
        prev = curr;
        curr = curr->next;
    }

    if (max_curr != nullptr)
    {
        if (max_prev == nullptr)
            *match_list = (*match_list)->next;
        else
            max_prev->next = max_curr->next;

        max_curr->next = match_free_list;
        match_free_list = max_curr;
        return max_curr->detector;
    }
    else
        return nullptr;
}

static void free_matched_list(ClientAppMatch** match_list)
{
    ClientAppMatch* cam;
    ClientAppMatch* tmp;

    cam = *match_list;
    while (cam)
    {
        tmp = cam;
        cam = tmp->next;
        tmp->next = match_free_list;
        match_free_list = tmp;
    }

    *match_list = nullptr;
}

/**
 * The process to determine the running client app given the packet data.
 *
 * @param p packet to process
 */
static void create_detector_candidates_list(Packet* p, const int /*direction*/, AppIdSession* asd)
{
    ClientAppMatch* match_list;

    if ( !p->dsize || asd->client_detector != nullptr || asd->client_candidates.size() )
        return;

    match_list = find_detector_candidates(p, asd->protocol);
    while (asd->client_candidates.size() < MAX_CANDIDATE_CLIENTS)
    {
        ClientDetector* cd = const_cast<ClientDetector*>(get_next_detector(&match_list));
        if (!cd)
            break;

        if ( asd->client_candidates.find(cd->name) == asd->client_candidates.end() )
            asd->client_candidates[cd->name] = cd;
    }

    free_matched_list(&match_list);
}

int get_detector_candidates_list(Packet* p, int direction, AppIdSession* asd)
{
    if (direction == APP_ID_FROM_INITIATOR)
    {
        /* get out if we've already tried to validate a client app */
        if (!asd->get_session_flags(APPID_SESSION_CLIENT_DETECTED))
            create_detector_candidates_list(p, direction, asd);
    }
    else if ( asd->rna_service_state != RNA_STATE_STATEFUL
        && asd->get_session_flags(APPID_SESSION_CLIENT_GETS_SERVER_PACKETS))
        create_detector_candidates_list(p, direction, asd);

    return APPID_SESSION_SUCCESS;
}

int ClientDiscovery::exec_client_detectors(AppIdSession& asd, Packet* p, int direction)
{
    int ret = APPID_INPROCESS;

    if (asd.client_detector != nullptr)
    {
        AppIdDiscoveryArgs disco_args(p->data, p->dsize, direction, &asd, p);
        ret = asd.client_detector->validate(disco_args);
        if (asd.session_logging_enabled)
            LogMessage("AppIdDbg %s %s client detector returned %d\n",
                asd.session_logging_id, asd.client_detector->name.c_str(), ret);
    }
    else
    {
        for ( auto& kv : asd.client_candidates )
        {
            AppIdDiscoveryArgs disco_args(p->data, p->dsize, direction, &asd, p);
            int result = kv.second->validate(disco_args);
            if (asd.session_logging_enabled)
                LogMessage("AppIdDbg %s %s client detector returned %d\n",
                    asd.session_logging_id, kv.second->name.c_str(), result);

            if (result == APPID_SUCCESS)
            {
                ret = APPID_SUCCESS;
                asd.client_detector = kv.second;
                asd.client_candidates.clear();
                break;
            }
            else if (result != APPID_INPROCESS)
            {
                asd.client_candidates.erase(kv.first);
            }
        }
    }

    return ret;
}

bool ClientDiscovery::do_client_discovery(AppIdSession& asd, int direction, Packet* p)
{
    bool isTpAppidDiscoveryDone = false;
    AppInfoTableEntry* entry;

    if (asd.rna_client_state != RNA_STATE_FINISHED)
    {
        Profile clientMatchPerfStats_profile_context(clientMatchPerfStats);
        uint32_t prevRnaClientState = asd.rna_client_state;
        bool was_http2 = asd.is_http2;
        bool was_service = asd.get_session_flags(APPID_SESSION_SERVICE_DETECTED) ? true : false;
        //decision to directly call validator or go through elaborate service_state tracking
        //is made once at the beginning of sesssion.
        if (asd.rna_client_state == RNA_STATE_NONE && p->dsize && direction ==
            APP_ID_FROM_INITIATOR)
        {
            if (p->flow->get_session_flags() & SSNFLAG_MIDSTREAM)
                asd.rna_client_state = RNA_STATE_FINISHED;
            else if (is_third_party_appid_available(asd.tpsession) && ( asd.tp_app_id >
                APP_ID_NONE )
                && ( asd.tp_app_id < SF_APPID_MAX ) )
            {
                entry = asd.app_info_mgr->get_app_info_entry(asd.tp_app_id);
                if ( entry && entry->client_detector
                    && ( ( entry->flags & APPINFO_FLAG_CLIENT_ADDITIONAL )
                    || ( ( entry->flags & APPINFO_FLAG_CLIENT_USER)
                    && asd.get_session_flags(APPID_SESSION_DISCOVER_USER) ) ) )
                {
                    //tp has positively identified appId, Dig deeper only if sourcefire
                    // detector identifies additional information
                    asd.client_detector = entry->client_detector;
                    asd.rna_client_state = RNA_STATE_DIRECT;
                }
                else
                {
                    asd.set_session_flags(APPID_SESSION_CLIENT_DETECTED);
                    asd.rna_client_state = RNA_STATE_FINISHED;
                }
            }
            else if (asd.get_session_flags(APPID_SESSION_HTTP_SESSION))
                asd.rna_client_state = RNA_STATE_FINISHED;
            else
                asd.rna_client_state = RNA_STATE_STATEFUL;
        }
        //stop rna inspection as soon as tp has classified a valid AppId later in the session
        if ((asd.rna_client_state == RNA_STATE_STATEFUL || asd.rna_client_state ==
            RNA_STATE_DIRECT)
            && asd.rna_client_state == prevRnaClientState && !asd.get_session_flags(
            APPID_SESSION_NO_TPI)
            && is_third_party_appid_available(asd.tpsession) && asd.tp_app_id > APP_ID_NONE &&
            asd.tp_app_id < SF_APPID_MAX)
        {
            entry = asd.app_info_mgr->get_app_info_entry(asd.tp_app_id);
            if (!(entry && entry->client_detector && entry->client_detector == asd.client_detector
                && (entry->flags & (APPINFO_FLAG_CLIENT_ADDITIONAL | APPINFO_FLAG_CLIENT_USER))))
            {
                asd.rna_client_state = RNA_STATE_FINISHED;
                asd.set_session_flags(APPID_SESSION_CLIENT_DETECTED);
            }
        }
        if (asd.rna_client_state == RNA_STATE_DIRECT)
        {
            int ret = APPID_INPROCESS;
            if (direction == APP_ID_FROM_INITIATOR)
            {
                /* get out if we've already tried to validate a client app */
                if (!asd.get_session_flags(APPID_SESSION_CLIENT_DETECTED))
                {
                    ret = exec_client_detectors(asd, p, direction);
                }
            }
            else if (asd.rna_service_state != RNA_STATE_STATEFUL
                && asd.get_session_flags(APPID_SESSION_CLIENT_GETS_SERVER_PACKETS))
            {
                ret = exec_client_detectors(asd, p, direction);
            }

            switch (ret)
            {
            case APPID_INPROCESS:
                break;
            default:
                asd.rna_client_state = RNA_STATE_FINISHED;
                break;
            }
        }
        else if (asd.rna_client_state == RNA_STATE_STATEFUL)
        {
            get_detector_candidates_list(p, direction, &asd);
            isTpAppidDiscoveryDone = true;
            if ( asd.client_candidates.size() )
            {
                int ret = 0;
                if (direction == APP_ID_FROM_INITIATOR)
                {
                    /* get out if we've already tried to validate a client app */
                    if (!asd.get_session_flags(APPID_SESSION_CLIENT_DETECTED))
                        ret = exec_client_detectors(asd, p, direction);
                }
                else if (asd.rna_service_state != RNA_STATE_STATEFUL
                    && asd.get_session_flags(APPID_SESSION_CLIENT_GETS_SERVER_PACKETS))
                    ret = exec_client_detectors(asd, p, direction);

                if (ret < 0)
                    asd.set_session_flags(APPID_SESSION_CLIENT_DETECTED);
            }
            else
                asd.set_session_flags(APPID_SESSION_CLIENT_DETECTED);
        }

        if (asd.session_logging_enabled)
            if (!was_http2 && asd.is_http2)
                LogMessage("AppIdDbg %s Got a preface for HTTP/2\n", asd.session_logging_id);

        if (!was_service && asd.get_session_flags(APPID_SESSION_SERVICE_DETECTED))
            asd.sync_with_snort_id(asd.serviceAppId, p);
    }

    return isTpAppidDiscoveryDone;
}
