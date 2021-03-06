/**
 * @file authentication.cpp
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

extern "C" {
#include <pjsip.h>
#include <pjlib-util.h>
#include <pjlib.h>
}

// Common STL includes.
#include <cassert>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <queue>
#include <string>
#include <boost/algorithm/string/predicate.hpp>
#include <json/reader.h>

#include "log.h"
#include "stack.h"
#include "sproutsasevent.h"
#include "pjutils.h"
#include "constants.h"
#include "analyticslogger.h"
#include "hssconnection.h"
#include "authentication.h"
#include "avstore.h"


//
// mod_auth authenticates SIP requests.  It must be inserted into the
// stack below the transaction layer.
//
static pj_bool_t authenticate_rx_request(pjsip_rx_data *rdata);
void log_sas_auth_not_needed_event(SAS::TrailId trail, std::string error_msg, int instance_id=0);

pjsip_module mod_auth =
{
  NULL, NULL,                         // prev, next
  pj_str("mod-auth"),                 // Name
  -1,                                 // Id
  PJSIP_MOD_PRIORITY_TSX_LAYER-1,     // Priority
  NULL,                               // load()
  NULL,                               // start()
  NULL,                               // stop()
  NULL,                               // unload()
  &authenticate_rx_request,           // on_rx_request()
  NULL,                               // on_rx_response()
  NULL,                               // on_tx_request()
  NULL,                               // on_tx_response()
  NULL,                               // on_tsx_state()
};

// Configuring PJSIP with a realm of "*" means that all realms are considered.
const pj_str_t WILDCARD_REALM = pj_str("*");

// Realm to use on AKA challenges.
static pj_str_t aka_realm;

// Connection to the HSS service for retrieving subscriber credentials.
static HSSConnection* hss;

static ChronosConnection* chronos;

// Factory for creating ACR messages for Rf billing.
static ACRFactory* acr_factory;


// AV store used to store Authentication Vectors while waiting for the
// client to respond to a challenge.
static AvStore* av_store;


// Analytics logger.
static AnalyticsLogger* analytics;


// PJSIP structure for control server authentication functions.
pjsip_auth_srv auth_srv;


/// Verifies that the supplied authentication vector is valid.
bool verify_auth_vector(Json::Value* av, const std::string& impi, SAS::TrailId trail)
{
  bool rc = true;

  // Check the AV is well formed.
  if (av->isMember("aka"))
  {
    // AKA is specified, check all the expected parameters are present.
    LOG_DEBUG("AKA specified");
    Json::Value& aka = (*av)["aka"];
    if ((!aka["challenge"].isString()) ||
        (!aka["response"].isString()) ||
        (!aka["cryptkey"].isString()) ||
        (!aka["integritykey"].isString()))
    {
      // Malformed AKA entry
      LOG_ERROR("Badly formed AKA authentication vector for %s\n%s",
                impi.c_str(), av->toStyledString().c_str());
      rc = false;

      SAS::Event event(trail, SASEvent::AUTHENTICATION_FAILED, 0);
      std::string error_msg = "AKA authentication vector is malformed: " + av->toStyledString();
      event.add_var_param(error_msg);
      SAS::report_event(event);
    }
  }
  else if (av->isMember("digest"))
  {
    // Digest is specified, check all the expected parameters are present.
    LOG_DEBUG("Digest specified");
    Json::Value& digest = (*av)["digest"];
    if ((!digest["realm"].isString()) ||
        (!digest["qop"].isString()) ||
        (!digest["ha1"].isString()))
    {
      // Malformed digest entry
      LOG_ERROR("Badly formed Digest authentication vector for %s\n%s",
                impi.c_str(), av->toStyledString().c_str());
      rc = false;

      SAS::Event event(trail, SASEvent::AUTHENTICATION_FAILED, 0);
      std::string error_msg = "Digest authentication vector is malformed: " + av->toStyledString();
      event.add_var_param(error_msg);
      SAS::report_event(event);
    }
  }
  else
  {
    // Neither AKA nor Digest information present.
    LOG_ERROR("No AKA or Digest object in authentication vector for %s\n%s",
              impi.c_str(), av->toStyledString().c_str());
    rc = false;

    SAS::Event event(trail, SASEvent::AUTHENTICATION_FAILED, 0);
    std::string error_msg = "Authentication vector is malformed: " + av->toStyledString();
    event.add_var_param(error_msg);
    SAS::report_event(event);
  }

  return rc;
}


pj_status_t user_lookup(pj_pool_t *pool,
                        const pjsip_auth_lookup_cred_param *param,
                        pjsip_cred_info *cred_info,
                        void* av_param)
{
  const pj_str_t* acc_name = &param->acc_name;
  const pj_str_t* realm = &param->realm;
  const pjsip_rx_data* rdata = param->rdata;
  SAS::TrailId trail = get_trail(rdata);

  pj_status_t status = PJSIP_EAUTHACCNOTFOUND;

  // Get the impi and the nonce.  There must be an authorization header otherwise
  // PJSIP wouldn't have called this method.
  std::string impi = PJUtils::pj_str_to_string(acc_name);
  pjsip_authorization_hdr* auth_hdr = (pjsip_authorization_hdr*)
           pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_AUTHORIZATION, NULL);
  std::string nonce = PJUtils::pj_str_to_string(&auth_hdr->credential.digest.nonce);

  // Get the Authentication Vector from the store.
  Json::Value* av = (Json::Value*)av_param;

  if (av == NULL)
  {
    LOG_WARNING("Received an authentication request for %s with nonce %s, but no matching AV found", impi.c_str(), nonce.c_str());
  }

  if ((av != NULL) &&
      (!verify_auth_vector(av, impi, trail)))
  {
    // Authentication vector is badly formed.
    av = NULL;                                                 // LCOV_EXCL_LINE
  }

  if (av != NULL)
  {
    pj_cstr(&cred_info->scheme, "digest");
    pj_strdup(pool, &cred_info->username, acc_name);
    if (av->isMember("aka"))
    {
      // AKA authentication.  The response in the AV must be used as a
      // plain-text password for the MD5 Digest computation.  Convert the text
      // into binary as this is what PJSIP is expecting.
      std::string response = (*av)["aka"]["response"].asString();
      std::string xres;
      for (size_t ii = 0; ii < response.length(); ii += 2)
      {
        xres.push_back((char)(pj_hex_digit_to_val(response[ii]) * 16 +
                              pj_hex_digit_to_val(response[ii+1])));
      }
      cred_info->data_type = PJSIP_CRED_DATA_PLAIN_PASSWD;
      pj_strdup2(pool, &cred_info->data, xres.c_str());
      LOG_DEBUG("Found AKA XRES = %.*s", cred_info->data.slen, cred_info->data.ptr);

      // Use default realm as it isn't specified in the AV.
      pj_strdup(pool, &cred_info->realm, realm);
      status = PJ_SUCCESS;
    }
    else if (av->isMember("digest"))
    {
      if (pj_strcmp2(realm, (*av)["digest"]["realm"].asCString()) == 0)
      {
        // Digest authentication, so ha1 field is hashed password.
        cred_info->data_type = PJSIP_CRED_DATA_DIGEST;
        pj_strdup2(pool, &cred_info->data, (*av)["digest"]["ha1"].asCString());
        cred_info->realm = *realm;
        LOG_DEBUG("Found Digest HA1 = %.*s", cred_info->data.slen, cred_info->data.ptr);
        status = PJ_SUCCESS;
      }
      else
      {
        // These credentials are for a different realm, so no credentials were
        // actually provided for us to check.
        status = PJSIP_EAUTHNOAUTH;
      }
    }

    correlate_branch_from_av(av, trail);
  }

  return status;
}

void create_challenge(pjsip_authorization_hdr* auth_hdr,
                      pj_bool_t stale,
                      std::string resync,
                      pjsip_rx_data* rdata,
                      pjsip_tx_data* tdata)
{
  // Get the public and private identities from the request.
  std::string impi;
  std::string impu;
  std::string nonce;

  PJUtils::get_impi_and_impu(rdata, impi, impu);
  // Set up the authorization type, following Annex P.4 of TS 33.203.  Currently
  // only support AKA and SIP Digest, so only implement the subset of steps
  // required to distinguish between the two.
  std::string auth_type;
  if (auth_hdr != NULL)
  {
    pjsip_param* integrity =
           pjsip_param_find(&auth_hdr->credential.digest.other_param,
                            &STR_INTEGRITY_PROTECTED);

    if ((integrity != NULL) &&
        ((pj_stricmp(&integrity->value, &STR_YES) == 0) ||
         (pj_stricmp(&integrity->value, &STR_NO) == 0)))
    {
      // Authentication scheme is AKA.
      auth_type = "aka";
    }
  }

  // Get the Authentication Vector from the HSS.
  Json::Value* av = NULL;
  HTTPCode http_code = hss->get_auth_vector(impi, impu, auth_type, resync, av, get_trail(rdata));

  if ((av != NULL) &&
      (!verify_auth_vector(av, impi, get_trail(rdata))))
  {
    // Authentication Vector is badly formed.
    delete av;
    av = NULL;
  }

  if (av != NULL)
  {
    // Retrieved a valid authentication vector, so generate the challenge.
    LOG_DEBUG("Valid AV - generate challenge");
    char buf[16];
    pj_str_t random;
    random.ptr = buf;
    random.slen = sizeof(buf);

    LOG_DEBUG("Create WWW-Authenticate header");
    pjsip_www_authenticate_hdr* hdr = pjsip_www_authenticate_hdr_create(tdata->pool);

    // Set up common fields for Digest and AKA cases (both are considered
    // Digest authentication).
    hdr->scheme = STR_DIGEST;

    if (av->isMember("aka"))
    {
      // AKA authentication.
      LOG_DEBUG("Add AKA information");

      SAS::Event event(get_trail(rdata), SASEvent::AUTHENTICATION_CHALLENGE, 0);
      std::string AKA = "AKA";
      event.add_var_param(AKA);
      SAS::report_event(event);

      Json::Value& aka = (*av)["aka"];

      // Use default realm for AKA as not specified in the AV.
      pj_strdup(tdata->pool, &hdr->challenge.digest.realm, &aka_realm);
      hdr->challenge.digest.algorithm = STR_AKAV1_MD5;
      nonce = aka["challenge"].asString();
      pj_strdup2(tdata->pool, &hdr->challenge.digest.nonce, nonce.c_str());
      pj_create_random_string(buf, sizeof(buf));
      pj_strdup(tdata->pool, &hdr->challenge.digest.opaque, &random);
      hdr->challenge.digest.qop = STR_AUTH;
      hdr->challenge.digest.stale = stale;

      // Add the cryptography key parameter.
      pjsip_param* ck_param = (pjsip_param*)pj_pool_alloc(tdata->pool, sizeof(pjsip_param));
      ck_param->name = STR_CK;
      std::string ck = "\"" + aka["cryptkey"].asString() + "\"";
      pj_strdup2(tdata->pool, &ck_param->value, ck.c_str());
      pj_list_insert_before(&hdr->challenge.digest.other_param, ck_param);

      // Add the integrity key parameter.
      pjsip_param* ik_param = (pjsip_param*)pj_pool_alloc(tdata->pool, sizeof(pjsip_param));
      ik_param->name = STR_IK;
      std::string ik = "\"" + aka["integritykey"].asString() + "\"";
      pj_strdup2(tdata->pool, &ik_param->value, ik.c_str());
      pj_list_insert_before(&hdr->challenge.digest.other_param, ik_param);
    }
    else
    {
      // Digest authentication.
      LOG_DEBUG("Add Digest information");

      SAS::Event event(get_trail(rdata), SASEvent::AUTHENTICATION_CHALLENGE, 0);
      std::string DIGEST = "DIGEST";
      event.add_var_param(DIGEST);
      SAS::report_event(event);

      Json::Value& digest = (*av)["digest"];
      pj_strdup2(tdata->pool, &hdr->challenge.digest.realm, digest["realm"].asCString());
      hdr->challenge.digest.algorithm = STR_MD5;
      pj_create_random_string(buf, sizeof(buf));
      nonce.assign(buf, sizeof(buf));
      pj_strdup(tdata->pool, &hdr->challenge.digest.nonce, &random);
      pj_create_random_string(buf, sizeof(buf));
      pj_strdup(tdata->pool, &hdr->challenge.digest.opaque, &random);
      pj_strdup2(tdata->pool, &hdr->challenge.digest.qop, digest["qop"].asCString());
      hdr->challenge.digest.stale = stale;
    }

    // Add the header to the message.
    pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)hdr);

    // Store the branch parameter in memcached for correlation purposes
    pjsip_via_hdr* via_hdr = (pjsip_via_hdr*)pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_VIA, NULL);
    std::string branch = (via_hdr != NULL) ? PJUtils::pj_str_to_string(&via_hdr->branch_param) : "";

    (*av)["branch"] = branch;

    // Write the authentication vector (as a JSON string) into the AV store.
    LOG_DEBUG("Write AV to store");
    uint64_t cas = 0;
    bool success = av_store->set_av(impi, nonce, av, cas, get_trail(rdata));
    if (success)
    {
      // We've written the AV into the store, so need to set a Chronos
      // timer so that an AUTHENTICATION_TIMEOUT SAR is sent to the
      // HSS when it expires.
      std::string timer_id;
      std::string chronos_body = "{\"impi\": \"" + impi + "\", \"impu\": \"" + impu +"\", \"nonce\": \"" + nonce +"\"}";
      LOG_DEBUG("Sending %s to Chronos to set AV timer", chronos_body.c_str());
      chronos->send_post(timer_id, 30, "/authentication-timeout", chronos_body, 0);
    }

    delete av;
  }
  else
  {
    std::string error_msg;

    // If we couldn't get the AV because a downstream node is overloaded then don't return
    // a 4xx error to the client.
    if ((http_code == HTTP_SERVER_UNAVAILABLE) || (http_code == HTTP_GATEWAY_TIMEOUT))
    {
      error_msg = "Downstream node is overloaded or unresponsive, unable to get Authentication vector";
      LOG_DEBUG(error_msg.c_str());
      tdata->msg->line.status.code = PJSIP_SC_SERVER_TIMEOUT;
      tdata->msg->line.status.reason = *pjsip_get_status_text(PJSIP_SC_SERVER_TIMEOUT);
    }
    else
    {
      error_msg = "Failed to get Authentication vector";
      LOG_DEBUG(error_msg.c_str());
      tdata->msg->line.status.code = PJSIP_SC_FORBIDDEN;
      tdata->msg->line.status.reason = *pjsip_get_status_text(PJSIP_SC_FORBIDDEN);
    }

    SAS::Event event(get_trail(rdata), SASEvent::AUTHENTICATION_FAILED, 0);
    event.add_var_param(error_msg);
    SAS::report_event(event);

    pjsip_tx_data_invalidate_msg(tdata);
  }
}


pj_bool_t authenticate_rx_request(pjsip_rx_data* rdata)
{
  pj_status_t status;
  std::string resync;

  SAS::TrailId trail = get_trail(rdata);

  if (rdata->tp_info.transport->local_name.port != stack_data.scscf_port)
  {
    // Request not received on S-CSCF port, so don't authenticate it.
    std::string error_msg = "Request wasn't received on S-CSCF port";
    log_sas_auth_not_needed_event(trail, error_msg);

    return PJ_FALSE;
  }

  if (rdata->msg_info.msg->line.req.method.id != PJSIP_REGISTER_METHOD)
  {
    // Non-REGISTER request, so don't do authentication as it must have come
    // from an authenticated or trusted source.
    std::string error_msg = "Request wasn't a REGISTER";
    log_sas_auth_not_needed_event(trail, error_msg);

    return PJ_FALSE;
  }

  // Authentication isn't required for emergency registrations. An emergency
  // registration is one where each Contact header contains 'sos' as the SIP
  // URI parameter.
  bool emergency_reg = true;

  pjsip_contact_hdr* contact_hdr = (pjsip_contact_hdr*)
                 pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_CONTACT, NULL);

  while ((contact_hdr != NULL) && (emergency_reg))
  {
    emergency_reg = PJUtils::is_emergency_registration(contact_hdr);
    contact_hdr = (pjsip_contact_hdr*) pjsip_msg_find_hdr(rdata->msg_info.msg,
                                                          PJSIP_H_CONTACT,
                                                          contact_hdr->next);
  }

  if (emergency_reg)
  {
    std::string error_msg = "Request is an emergency REGISTER";
    log_sas_auth_not_needed_event(trail, error_msg);

    return PJ_FALSE;
  }

  // Check to see if the request has already been integrity protected?
  pjsip_authorization_hdr* auth_hdr = (pjsip_authorization_hdr*)
           pjsip_msg_find_hdr(rdata->msg_info.msg, PJSIP_H_AUTHORIZATION, NULL);

  if (auth_hdr != NULL)
  {
    // There is an authorization header, so check for the integrity-protected
    // indication.
    LOG_DEBUG("Authorization header in request");
    pjsip_param* integrity =
           pjsip_param_find(&auth_hdr->credential.digest.other_param,
                            &STR_INTEGRITY_PROTECTED);

    if ((integrity != NULL) &&
        ((pj_stricmp(&integrity->value, &STR_TLS_YES) == 0) ||
         (pj_stricmp(&integrity->value, &STR_IP_ASSOC_YES) == 0)))
    {
      // The integrity protected indicator is included and set to tls-yes or
      // ip-assoc-yes.  This indicates the client has already been authenticated
      // so we will accept this REGISTER even if there is a challenge response.
      // (Values of tls-pending or ip-assoc-pending indicate the challenge
      // should be checked.)
      LOG_INFO("SIP Digest authenticated request integrity protected by edge proxy");

      std::string error_msg = "SIP digest authenticated request integrity protected by edge proxy";
      log_sas_auth_not_needed_event(trail, error_msg);

      return PJ_FALSE;
    }
    else if ((integrity != NULL) &&
             (pj_stricmp(&integrity->value, &STR_YES) == 0) &&
             (auth_hdr->credential.digest.response.slen == 0))
    {
      // The integrity protected indicator is include and set to yes.  This
      // indicates that AKA authentication is in use and the REGISTER was
      // received on an integrity protected channel, so we will let the
      // request through if there is no challenge response, but must check
      // the challenge response if included.
      LOG_INFO("AKA authenticated request integrity protected by edge proxy");

      std::string error_msg = "AKA authenticated request integrity protected by edge proxy";
      log_sas_auth_not_needed_event(trail, error_msg);

      return PJ_FALSE;
    }
  }

  int sc = PJSIP_SC_UNAUTHORIZED;
  status = PJSIP_EAUTHNOAUTH;

  if ((auth_hdr != NULL) &&
      (auth_hdr->credential.digest.response.slen != 0))
  {
    std::string impi = PJUtils::pj_str_to_string(&auth_hdr->credential.digest.username);
    std::string nonce = PJUtils::pj_str_to_string(&auth_hdr->credential.digest.nonce);
    uint64_t cas = 0;

    Json::Value* av = av_store->get_av(impi, nonce, cas, trail);

    // Request contains a response to a previous challenge, so pass it to
    // the authentication module to verify.
    LOG_DEBUG("Verify authentication information in request");
    status = pjsip_auth_srv_verify2(&auth_srv, rdata, &sc, (void*)av);

    if (status == PJ_SUCCESS)
    {
      // The authentication information in the request was verified.
      LOG_DEBUG("Request authenticated successfully");

      SAS::Event event(trail, SASEvent::AUTHENTICATION_SUCCESS, 0);
      SAS::report_event(event);

      (*av)["tombstone"] = Json::Value("true");
      bool rc = av_store->set_av(impi, nonce, av, cas, trail);

      if (!rc)
      {
        // LCOV_EXCL_START
        LOG_ERROR("Tried to tombstone AV for %s/%s after processing an authentication, but failed",
                  impi.c_str(),
                  nonce.c_str());
        // LCOV_EXCL_STOP
      }

      // If doing AKA authentication, check for an AUTS parameter.  We only
      // check this if the request authenticated as actioning it otherwise
      // is a potential denial of service attack.
      if (!pj_strcmp(&auth_hdr->credential.digest.algorithm, &STR_AKAV1_MD5))
      {
        LOG_DEBUG("AKA authentication so check for client resync request");
        pjsip_param* p = pjsip_param_find(&auth_hdr->credential.digest.other_param,
                                          &STR_AUTS);

        if (p != NULL)
        {
          // Found AUTS parameter, so UE is requesting a resync.  We need to
          // redo the authentication, passing an auts parameter to the HSS
          // comprising the first 16 octets of the nonce (RAND) and the 14
          // octets of the auts parameter.  (See TS 33.203 and table 6.3.3 of
          // TS 29.228 for details.)
          LOG_DEBUG("AKA SQN resync request from UE");
          std::string auts = PJUtils::pj_str_to_string(&p->value);
          std::string nonce = PJUtils::pj_str_to_string(&auth_hdr->credential.digest.nonce);
          if ((auts.length() != 14) ||
              (nonce.length() != 32))
          {
            // AUTS and/or nonce are malformed, so reject the request.
            LOG_WARNING("Invalid auts/nonce on resync request from private identity %.*s",
                        auth_hdr->credential.digest.username.slen,
                        auth_hdr->credential.digest.username.ptr);
            status = PJSIP_EAUTHINAKACRED;
            sc = PJSIP_SC_FORBIDDEN;
          }
          else
          {
            // auts and nonce are as expected, so create the resync string
            // that needs to be passed to the HSS, and act as if no
            // authentication information was received.
            resync = nonce.substr(0,16) + auts;
            status = PJSIP_EAUTHNOAUTH;
            sc = PJSIP_SC_UNAUTHORIZED;
          }
        }
      }

      if (status == PJ_SUCCESS)
      {
        // Request authentication completed, so let the message through to
        // other modules.
        delete av;
        return PJ_FALSE;
      }
    }
    delete av;
  }


  // The message either has insufficient authentication information, or
  // has failed authentication.  In either case, the message will be
  // absorbed and responded to by the authentication module, so we need to
  // add SAS markers so the trail will become searchable.
  SAS::Marker start_marker(trail, MARKER_ID_START, 1u);
  SAS::report_marker(start_marker);

  PJUtils::report_sas_to_from_markers(trail, rdata->msg_info.msg);
  PJUtils::mark_sas_call_branch_ids(trail, NULL, rdata->msg_info.msg);

  // Add a SAS end marker
  SAS::Marker end_marker(trail, MARKER_ID_END, 1u);
  SAS::report_marker(end_marker);

  // Create an ACR for the message and pass the request to it.  Role is always
  // considered originating for a REGISTER request.
  ACR* acr = acr_factory->get_acr(trail,
                                  CALLING_PARTY,
                                  NODE_ROLE_ORIGINATING);
  acr->rx_request(rdata->msg_info.msg, rdata->pkt_info.timestamp);

  pjsip_tx_data* tdata;

  if ((status == PJSIP_EAUTHNOAUTH) ||
      (status == PJSIP_EAUTHACCNOTFOUND))
  {
    // No authorization information in request, or no authentication vector
    // found in the store (so request is likely stale), so must issue
    // challenge.
    LOG_DEBUG("No authentication information in request or stale nonce, so reject with challenge");
    pj_bool_t stale = (status == PJSIP_EAUTHACCNOTFOUND);

    sc = PJSIP_SC_UNAUTHORIZED;
    status = PJUtils::create_response(stack_data.endpt, rdata, sc, NULL, &tdata);

    if (status != PJ_SUCCESS)
    {
      // Failed to create a response.  This really shouldn't happen, but there
      // is nothing else we can do.
      // LCOV_EXCL_START
      delete acr;
      return PJ_TRUE;
      // LCOV_EXCL_STOP
    }

    create_challenge(auth_hdr, stale, resync, rdata, tdata);
  }
  else
  {
    // Authentication failed.
    std::string error_msg = PJUtils::pj_status_to_string(status);

    LOG_ERROR("Authentication failed, %s", error_msg.c_str());

    SAS::Event event(trail, SASEvent::AUTHENTICATION_FAILED, 0);
    event.add_var_param(error_msg);
    SAS::report_event(event);

    if (sc != PJSIP_SC_UNAUTHORIZED)
    {
      // Notify Homestead and the HSS that this authentication attempt
      // has definitively failed.
      std::string impi;
      std::string impu;

      PJUtils::get_impi_and_impu(rdata, impi, impu);

      hss->update_registration_state(impu, impi, HSSConnection::AUTH_FAIL, 0);
    }

    if (analytics != NULL)
    {
      analytics->auth_failure(PJUtils::pj_str_to_string(&auth_hdr->credential.digest.username),
                              PJUtils::public_id_from_uri((pjsip_uri*)pjsip_uri_get_uri(PJSIP_MSG_TO_HDR(rdata->msg_info.msg)->uri)));
    }

    // @TODO - need more diagnostics here so we can identify and flag
    // attacks.

    status = PJUtils::create_response(stack_data.endpt, rdata, sc, NULL, &tdata);
    if (status != PJ_SUCCESS)
    {
      // Failed to create a response.  This really shouldn't happen, but there
      // is nothing else we can do.
      // LCOV_EXCL_START
      delete acr;
      return PJ_TRUE;
      // LCOV_EXCL_STOP
    }
  }

  acr->tx_response(tdata->msg);

  status = pjsip_endpt_send_response2(stack_data.endpt, rdata, tdata, NULL, NULL);

  // Send the ACR.
  acr->send_message();
  delete acr;

  return PJ_TRUE;
}


pj_status_t init_authentication(const std::string& realm_name,
                                AvStore* avstore,
                                HSSConnection* hss_connection,
                                ChronosConnection* chronos_connection,
                                ACRFactory* rfacr_factory,
                                AnalyticsLogger* analytics_logger)
{
  pj_status_t status;

  aka_realm = (realm_name != "") ? pj_strdup3(stack_data.pool, realm_name.c_str()) : stack_data.local_host;
  av_store = avstore;
  hss = hss_connection;
  chronos = chronos_connection;
  acr_factory = rfacr_factory;
  analytics = analytics_logger;

  // Register the authentication module.  This needs to be in the stack
  // before the transaction layer.
  status = pjsip_endpt_register_module(stack_data.endpt, &mod_auth);

  // Initialize the authorization server.
  pjsip_auth_srv_init_param params;
  params.realm = &WILDCARD_REALM;
  params.lookup3 = user_lookup;
  params.options = 0;
  status = pjsip_auth_srv_init2(stack_data.pool, &auth_srv, &params);

  return status;
}


void destroy_authentication()
{
  pjsip_endpt_unregister_module(stack_data.endpt, &mod_auth);
}

void log_sas_auth_not_needed_event(SAS::TrailId trail, std::string error_msg, int instance_id)
{
  SAS::Event event(trail, SASEvent::AUTHENTICATION_NOT_NEEDED, instance_id);
  event.add_var_param(error_msg);
  SAS::report_event(event);
}

