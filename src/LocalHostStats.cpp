/*
 *
 * (C) 2013-21 - ntop.org
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "ntop_includes.h"

/* *************************************** */

LocalHostStats::LocalHostStats(Host *_host) : HostStats(_host) {
  top_sites = new (std::nothrow) FrequentStringItems(HOST_SITES_TOP_NUMBER);
  old_sites = NULL;
  dns  = new (std::nothrow) DnsStats();
  http = new (std::nothrow) HTTPstats(_host);
  icmp = new (std::nothrow) ICMPstats();
  nextSitesUpdate = 0, nextContactsUpdate = time(NULL)+HOST_CONTACTS_REFRESH;
  num_contacts_as_cli = num_contacts_as_srv = 0;
  current_cycle = 0;

  if (_host != NULL)
    removeRedisSitesKey(_host);
  
  num_contacted_hosts_as_client.init(8);       /* 128 bytes */
  num_host_contacts_as_server.init(8);         /* 128 bytes */
  num_contacted_services_as_client.init(8);    /* 128 bytes */
  num_contacted_ports_as_client.init(4);       /* 16 bytes  */
  num_host_contacted_ports_as_server.init(4);  /* 16 bytes  */
  contacts_as_cli.init(4);                     /* 16 bytes  */
  contacts_as_srv.init(4);                     /* 16 bytes  */
}

/* *************************************** */

LocalHostStats::LocalHostStats(LocalHostStats &s) : HostStats(s) {
  top_sites = new (std::nothrow) FrequentStringItems(HOST_SITES_TOP_NUMBER);
  old_sites = NULL;
  dns = s.getDNSstats() ? new (std::nothrow) DnsStats(*s.getDNSstats()) : NULL;
  http = NULL;
  icmp = NULL;
  nextSitesUpdate = 0, nextContactsUpdate = time(NULL)+HOST_CONTACTS_REFRESH;
  num_contacts_as_cli = num_contacts_as_srv = 0;

  if (host != NULL)
    removeRedisSitesKey(host);
}

/* *************************************** */

LocalHostStats::~LocalHostStats() {
  if(top_sites)       delete top_sites;
  if(old_sites)       free(old_sites);
  if(dns)             delete dns;
  if(http)            delete http;
  if(icmp)            delete icmp;

  if (host != NULL)
    addRedisSitesKey(host);
  
}

/* *************************************** */

void LocalHostStats::incrVisitedWebSite(char *hostname) {
  u_int ip4_0 = 0, ip4_1 = 0, ip4_2 = 0, ip4_3 = 0;
  char *firstdot = NULL, *nextdot = NULL;

  if(top_sites
     && ntop->getPrefs()->are_top_talkers_enabled()
     && (strstr(hostname, "in-addr.arpa") == NULL)
     && (sscanf(hostname, "%u.%u.%u.%u", &ip4_0, &ip4_1, &ip4_2, &ip4_3) != 4)
     ) {
    if(ntop->isATrackerHost(hostname)) {
      ntop->getTrace()->traceEvent(TRACE_INFO, "[TRACKER] %s", hostname);
      return; /* Ignore trackers */
    }

    firstdot = strchr(hostname, '.');

    if(firstdot)
      nextdot = strchr(&firstdot[1], '.');

    top_sites->add(nextdot ? &firstdot[1] : hostname, 1);
    iface->incrVisitedWebSite(hostname);
  }
}

/* *************************************** */

void LocalHostStats::updateStats(const struct timeval *tv) {
  HostStats::updateStats(tv);

  if(dns)  dns->updateStats(tv);
  if(icmp) icmp->updateStats(tv);
  if(http) http->updateStats(tv);

  if(tv->tv_sec >= nextContactsUpdate) {
    updateHostContacts();
    nextContactsUpdate = tv->tv_sec+HOST_CONTACTS_REFRESH;
  }
  
  if(top_sites && ntop->getPrefs()->are_top_talkers_enabled() && (tv->tv_sec >= nextSitesUpdate)) {
    if(nextSitesUpdate > 0) {
      if(old_sites) {
        if(host != NULL)
          this->saveOldSites();
	      free(old_sites);
      }
      if(top_sites->getSize())
        old_sites = top_sites->json();
    }

    nextSitesUpdate = tv->tv_sec + HOST_SITES_REFRESH;
  }
}

/* *************************************** */

void LocalHostStats::updateHostContacts() {
  num_contacts_as_cli = contacts_as_cli.getEstimate(), num_contacts_as_srv = contacts_as_srv.getEstimate();
  contacts_as_cli.reset(), contacts_as_srv.reset();
}

/* *************************************** */

void LocalHostStats::getJSONObject(json_object *my_object, DetailsLevel details_level) {
  HostStats::getJSONObject(my_object, details_level);

  if(dns)  json_object_object_add(my_object, "dns", dns->getJSONObject());
  if(http) json_object_object_add(my_object, "http", http->getJSONObject());

  /* UDP stats */
  if(udp_sent_unicast) json_object_object_add(my_object, "udpBytesSent.unicast", json_object_new_int64(udp_sent_unicast));
  if(udp_sent_non_unicast) json_object_object_add(my_object, "udpBytesSent.non_unicast", json_object_new_int64(udp_sent_non_unicast));
}

/* *************************************** */

void LocalHostStats::lua(lua_State* vm, bool mask_host, DetailsLevel details_level) {
  HostStats::lua(vm, mask_host, details_level);

  if((!mask_host) && top_sites && ntop->getPrefs()->are_top_talkers_enabled()) {
    if(top_sites) {
      char *cur_sites = top_sites->json();
      lua_push_str_table_entry(vm, "sites", cur_sites ? cur_sites : (char*)"{}");
      if(cur_sites) free(cur_sites);
    }
    if(old_sites)
      lua_push_str_table_entry(vm, "sites.old", old_sites ? old_sites : (char*)"{}");
  }

  if(details_level >= details_high) {
    luaICMP(vm,host->get_ip()->isIPv4(),true);
    luaDNS(vm, true);
    luaHTTP(vm);
    
    /* Contacts */
    lua_newtable(vm);
      
    lua_push_int32_table_entry(vm, "num_contacted_hosts_as_client",
			       num_contacted_hosts_as_client.getEstimate()); 
    lua_push_int32_table_entry(vm, "num_host_contacts_as_server",
			       num_host_contacts_as_server.getEstimate());
    lua_push_int32_table_entry(vm, "num_contacted_services_as_client",
			       num_contacted_services_as_client.getEstimate());
    lua_push_int32_table_entry(vm, "num_contacted_ports_as_client",
			       num_contacted_ports_as_client.getEstimate());
    lua_push_int32_table_entry(vm, "num_host_contacted_ports_as_server",
			       num_host_contacted_ports_as_server.getEstimate());
      
    lua_pushstring(vm, "cardinality");
    lua_insert(vm, -2);
    lua_settable(vm, -3);
    
  }  
}

/* *************************************** */

void LocalHostStats::deserialize(json_object *o) {
  json_object *obj;

  HostStats::deserialize(o);

  l4stats.deserialize(o);

  /* packet stats */
  if(json_object_object_get_ex(o, "pktStats.sent", &obj))  sent_stats.deserialize(obj);
  if(json_object_object_get_ex(o, "pktStats.recv", &obj))  recv_stats.deserialize(obj);

  /* UDP stats */
  if(json_object_object_get_ex(o, "udpBytesSent.unicast", &obj))      udp_sent_unicast = json_object_get_int64(obj);
  if(json_object_object_get_ex(o, "udpBytesSent.non_unicast", &obj))  udp_sent_non_unicast = json_object_get_int64(obj);

  /* TCP packet stats */
  if(json_object_object_get_ex(o, "tcpPacketStats.sent", &obj))  tcp_packet_stats_sent.deserialize(obj);
  if(json_object_object_get_ex(o, "tcpPacketStats.recv", &obj))  tcp_packet_stats_rcvd.deserialize(obj);

  GenericTrafficElement::deserialize(o, iface);

  if(json_object_object_get_ex(o, "total_activity_time", &obj))  total_activity_time = json_object_get_int(obj);

  if(json_object_object_get_ex(o, "dns", &obj)) {
    if(dns) dns->deserialize(obj);
  }

  if(json_object_object_get_ex(o, "http", &obj)) {
    if(http) http->deserialize(obj);
  }

  if(json_object_object_get_ex(o, "pktStats.sent", &obj)) sent_stats.deserialize(obj);
  if(json_object_object_get_ex(o, "pktStats.recv", &obj)) recv_stats.deserialize(obj);

  if(json_object_object_get_ex(o, "flows.as_client", &obj))  total_num_flows_as_client = json_object_get_int(obj);
  if(json_object_object_get_ex(o, "flows.as_server", &obj))  total_num_flows_as_server = json_object_get_int(obj);
  if(json_object_object_get_ex(o, "alerted_flows.as_client", &obj))  alerted_flows_as_client = json_object_get_int(obj);
  if(json_object_object_get_ex(o, "alerted_flows.as_server", &obj))  alerted_flows_as_server = json_object_get_int(obj);
  if(json_object_object_get_ex(o, "unreachable_flows.as_client", &obj))  unreachable_flows_as_client = json_object_get_int(obj);
  if(json_object_object_get_ex(o, "unreachable_flows.as_server", &obj))  unreachable_flows_as_server = json_object_get_int(obj);
  if(json_object_object_get_ex(o, "host_unreachable_flows.as_client", &obj))  host_unreachable_flows_as_client = json_object_get_int(obj);
  if(json_object_object_get_ex(o, "host_unreachable_flows.as_server", &obj))  host_unreachable_flows_as_server = json_object_get_int(obj);
  /* NOTE: total_alerts currently not (de)serialized */

  /* Restores possibly checkpointed data */
  checkpoints.sent_bytes = getNumBytesSent();
  checkpoints.rcvd_bytes = getNumBytesRcvd();
}

/* *************************************** */

void LocalHostStats::lua_get_timeseries(lua_State* vm) {
  luaStats(vm, iface, true /* host details */, true /* verbose */, true /* tsLua */);

  tcp_packet_stats_sent.lua(vm, "tcpPacketStats.sent");
  tcp_packet_stats_rcvd.lua(vm, "tcpPacketStats.rcvd");

  if(dns) dns->lua(vm, false /* NOT verbose */);

  if(icmp) {
    struct ts_icmp_stats icmp_s;
    icmp->getTsStats(&icmp_s);

    lua_push_uint64_table_entry(vm, "icmp.echo_pkts_sent", icmp_s.echo_packets_sent);
    lua_push_uint64_table_entry(vm, "icmp.echo_pkts_rcvd", icmp_s.echo_packets_rcvd);
    lua_push_uint64_table_entry(vm, "icmp.echo_reply_pkts_sent", icmp_s.echo_reply_packets_sent);
    lua_push_uint64_table_entry(vm, "icmp.echo_reply_pkts_rcvd", icmp_s.echo_reply_packets_rcvd);
  }
}

/* *************************************** */

bool LocalHostStats::hasAnomalies(time_t when) {
  bool ret = false;

  if(dns)  ret |= dns->hasAnomalies(when);
  if(icmp) ret |= icmp->hasAnomalies(when);

  return ret;
}

/* *************************************** */

void LocalHostStats::luaAnomalies(lua_State* vm, time_t when) {
  if(dns)  dns->luaAnomalies(vm, when);
  if(icmp) icmp->luaAnomalies(vm, when);
}

/* *************************************** */

void LocalHostStats::incStats(time_t when, u_int8_t l4_proto,
			      u_int ndpi_proto, ndpi_protocol_category_t ndpi_category,
			      custom_app_t custom_app,
			      u_int64_t sent_packets, u_int64_t sent_bytes, u_int64_t sent_goodput_bytes,
			      u_int64_t rcvd_packets, u_int64_t rcvd_bytes, u_int64_t rcvd_goodput_bytes,
			      bool peer_is_unicast) {
  HostStats::incStats(when, l4_proto, ndpi_proto, ndpi_category, custom_app,
		      sent_packets, sent_bytes, sent_goodput_bytes,
		      rcvd_packets, rcvd_bytes, rcvd_goodput_bytes, peer_is_unicast);

  if(l4_proto == IPPROTO_UDP) {
    if(peer_is_unicast)
      udp_sent_unicast += sent_bytes;
    else
      udp_sent_non_unicast += sent_bytes;
  }
}

/* *************************************** */

void LocalHostStats::getCurrentTime(struct tm *t_now) {
  time_t now = time(NULL); 
  memset(t_now, 0, sizeof(*t_now));
  localtime_r(&now, t_now);
}

/* *************************************** */

void LocalHostStats::addRemoveRedisKey(Host *host, char *host_buf, struct tm *t_now, bool push) {
  char redis_hour_key[256], redis_daily_key[256];
  int iface, vlan = host->get_vlan_id();

  if(!host->getInterface())
    return;

  iface = host->getInterface()->get_id();

  snprintf(redis_hour_key, sizeof(redis_hour_key), "%s@%u_%u_%d_%u", host_buf, vlan, iface, t_now->tm_mday, t_now->tm_hour);
  snprintf(redis_daily_key, sizeof(redis_daily_key), "%s@%u_%u_%d", host_buf, vlan, iface, t_now->tm_mday);

  if(push) {
    ntop->getRedis()->lpush((char*) HASHKEY_LOCAL_HOSTS_TOP_SITES_HOUR_KEYS_PUSHED, redis_hour_key, 3600);
    ntop->getRedis()->lpush((char*) HASHKEY_LOCAL_HOSTS_TOP_SITES_DAY_KEYS_PUSHED, redis_daily_key, 3600);
  }
  else {
    ntop->getRedis()->lrem((char*) HASHKEY_LOCAL_HOSTS_TOP_SITES_HOUR_KEYS_PUSHED, redis_hour_key);
    ntop->getRedis()->lrem((char*) HASHKEY_LOCAL_HOSTS_TOP_SITES_DAY_KEYS_PUSHED, redis_daily_key);
  }
}

/* *************************************** */

void LocalHostStats::saveOldSites() {
  char host_buf[128], redis_key[256];
  u_int32_t iface, vlan = host->get_vlan_id();
  int minute = 0;
  struct tm t_now;

  if(!old_sites)
    return;
  
  if(!host->getInterface())
    return;

  if(!host->get_mac() && !host->get_ip())
    return;

  host->get_tskey(host_buf, sizeof(host_buf));

  getCurrentTime(&t_now);

  minute = t_now.tm_min - (t_now.tm_min % 5);
  iface  = host->getInterface()->get_id();

  /* String like `ntopng.cache_1.1.1.1@2_1_17_11_45` */
  /* An other way is to use the localtime_r and compose the string like `ntopng.cache_1.1.1.1@2_1_1609761600` */
  snprintf(redis_key, sizeof(redis_key), "%s_%s@%d_%d_%d_%d_%d", (char*) NTOPNG_CACHE_PREFIX, 
            host_buf, vlan, iface, t_now.tm_mday, t_now.tm_hour, minute);
  
  ntop->getRedis()->set(redis_key , old_sites, 7200);

  if (minute == 0 && current_cycle > 0) {
    char hour_done[256];
    int hour = 0;

    if (t_now.tm_hour == 0) 
      hour = 23;
    else
      hour = t_now.tm_hour - 1;

    /* List key = ntopng.cache.top_sites_hour_done | value = 1.1.1.1@2_1_17_11 */ 
    snprintf(hour_done, sizeof(hour_done), "%s@%d_%d_%d_%d", host_buf, vlan, iface, t_now.tm_mday, hour);
    ntop->getRedis()->lpush((char*) HASHKEY_LOCAL_HOSTS_TOP_SITES_HOUR_KEYS_PUSHED, hour_done, 3600);

    current_cycle = 0;
  } else 
    current_cycle++;
}

/* *************************************** */

void LocalHostStats::removeRedisSitesKey(Host *host) {
  char host_buf[128];
  struct tm t_now;

  if(!host->get_mac() && !host->get_ip())
    return;

  host->get_tskey(host_buf, sizeof(host_buf));

  getCurrentTime(&t_now);
  addRemoveRedisKey(host, host_buf, &t_now, false);
}

/* *************************************** */
  
void LocalHostStats::addRedisSitesKey(Host *host) {
  char host_buf[128];
  struct tm t_now;

  if(!host->get_mac() && !host->get_ip())
    return;

  host->get_tskey(host_buf, sizeof(host_buf));

  getCurrentTime(&t_now);   
  addRemoveRedisKey(host, host_buf, &t_now, true);
}

/* *************************************** */
