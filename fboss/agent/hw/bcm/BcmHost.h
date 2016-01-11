/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

extern "C" {
#include <opennsl/types.h>
#include <opennsl/l3.h>
}

#include <folly/IPAddress.h>
#include <folly/MacAddress.h>
#include "fboss/agent/types.h"
#include "fboss/agent/hw/bcm/BcmEgress.h"
#include "fboss/agent/state/RouteForwardInfo.h"
#include "fboss/agent/state/NeighborEntry.h"

#include <boost/container/flat_map.hpp>

namespace facebook { namespace fboss {

class BcmEcmpEgress;
class BcmEgress;
class BcmSwitch;

class BcmHost {
 public:
  BcmHost(
      const BcmSwitch* hw, opennsl_vrf_t vrf, const folly::IPAddress& addr,
      opennsl_if_t referenced_egress = BcmEgressBase::INVALID);
  virtual ~BcmHost();
  bool isProgrammed() const {
    return added_;
  }
  /*
   * program* apis get called only when for non host
   * route entries (which provide a L2 mapping for
   * a IP). Here we need to do 2 things
   * a) Add egress entry.
   * b) Add a host entry.
   * For host routes, we only need to do b) since we use
   * a already created egress entry
   */
  void program(opennsl_if_t intf, folly::MacAddress mac,
      opennsl_port_t port) {
    return program(intf, &mac, port, NEXTHOPS);
  }
  void programToCPU(opennsl_if_t intf) {
    return program(intf, nullptr, 0, TO_CPU);
  }
  void programToDrop(opennsl_if_t intf) {
    return program(intf, nullptr, 0, DROP);
  }
  opennsl_if_t getEgressId() const {
    return egressId_;
  }

  bool getAndClearHitBit() const;
  void addBcmHost(bool isMultipath = false);
 private:
  // no copy or assignment
  BcmHost(BcmHost const &) = delete;
  BcmHost& operator=(BcmHost const &) = delete;
  void program(opennsl_if_t intf, const folly::MacAddress *mac,
               opennsl_port_t port, RouteForwardAction action);
  void initHostCommon(opennsl_l3_host_t *host) const;
  const BcmSwitch* hw_;
  opennsl_vrf_t vrf_;
  folly::IPAddress addr_;
  opennsl_if_t egressId_{BcmEgressBase::INVALID};
  bool added_{false}; // if added to the HW host(ARP) table or not
};

/**
 * Class to abstract ECMP path
 *
 * There are 2 use cases for BCM Ecmp host
 * a) As a collection of BcmHost entries - Unlike BcmHost, in this case
 * BcmEcmpHost does not have its own HW programming. It functions as
 * a SW object which refers to one or multiple BcmHost objects.
 * b) As a object representing a host route. In this case the BcmEcmpHost
 * simply references another egress entry (which maybe either BcmEgress
 * or BcmEcmpEgress).
 */
class BcmEcmpHost {
 public:
  BcmEcmpHost(const BcmSwitch* hw, opennsl_vrf_t vrf,
              const RouteForwardNexthops& fwd);
  virtual ~BcmEcmpHost();
  opennsl_if_t getEgressId() const {
    return egressId_;
  }
 private:
  const BcmSwitch* hw_;
  opennsl_vrf_t vrf_;
  /**
   * The egress ID for this ECMP host
   *
   * If there is only one entry in 'fwd', there will be one BcmHost object
   * created. The egress ID is that host's egress ID.
   * Otherwise, one BcmEcmpEgress object is created. The egress ID is the one
   * from this ECMP egress object. In the latter case, ecmpEgressId_ will also
   * be set and both egressId_ and ecmpEgressId_ will be that of the ecmp egress
   * object.
   */
  opennsl_if_t egressId_{BcmEgressBase::INVALID};
  opennsl_if_t ecmpEgressId_{BcmEgressBase::INVALID};
  RouteForwardNexthops fwd_;
};

class BcmHostTable {
 public:
  explicit BcmHostTable(const BcmSwitch *hw);
  virtual ~BcmHostTable();
  // throw an exception if not found
  BcmHost* getBcmHost(opennsl_vrf_t vrf, const folly::IPAddress& addr) const;
  BcmEcmpHost* getBcmEcmpHost(
      opennsl_vrf_t vrf, const RouteForwardNexthops& fwd) const;
  // return nullptr if not found
  BcmHost* getBcmHostIf(
      opennsl_vrf_t vrf, const folly::IPAddress& addr) const;
  BcmEcmpHost* getBcmEcmpHostIf(
      opennsl_vrf_t vrf, const RouteForwardNexthops&) const;
  /*
   * The following functions will modify the object. They rely on the global
   * HW update lock in BcmSwitch::lock_ for the protection.
   *
   * BcmHostTable maintains a reference counter for each BcmHost/BcmEcmpHost
   * entry allocated.
   */
  /**
   * Allocates a new BcmHost/BcmEcmpHost if no such one exists. For the existing
   * entry, incRefOrCreateBcmHost() will increase the reference counter by 1.
   *
   * When a new BcmHost is created, the programming to HW is not performed,
   * until explicit BcmHost::program() or BcmHost::programToCPU() is called.
   *
   * @return The BcmHost/BcmEcmpHost pointer just created or found.
   */
  BcmHost* incRefOrCreateBcmHost(
      opennsl_vrf_t vrf, const folly::IPAddress& addr);
  BcmHost* incRefOrCreateBcmHost(
      opennsl_vrf_t vrf, const folly::IPAddress& addr, opennsl_if_t egressId);
  BcmEcmpHost* incRefOrCreateBcmEcmpHost(
      opennsl_vrf_t vrf, const RouteForwardNexthops& fwd);

  /**
   * Decrease an existing BcmHost/BcmEcmpHost entry's reference counter by 1.
   * Only until the reference counter is 0, the BcmHost/BcmEcmpHost entry
   * is deleted.
   *
   * @return nullptr, if the BcmHost/BcmEcmpHost entry is deleted
   * @retrun the BcmHost/BcmEcmpHost object that has reference counter
   *         decreased by 1, but the object is still valid as it is
   *         still referred in somewhere else
   */
  BcmHost* derefBcmHost(
      opennsl_vrf_t vrf, const folly::IPAddress& addr) noexcept;
  BcmEcmpHost* derefBcmEcmpHost(opennsl_vrf_t vrf,
                                const RouteForwardNexthops& fwd) noexcept;

  /*
   * APIs to manage egress objects. Multiple host entries can point
   * to a egress object. Lifetime of these egress objects is thus
   * managed via a reference count of hosts pointing to these
   * egress objects. Once the last host pointing to a egress object
   * goes away, the egress object is deleted.
   */
  void insertBcmEgress(std::unique_ptr<BcmEgressBase> egress);
  BcmEgressBase* incEgressReference(opennsl_if_t egressId);
  BcmEgressBase* derefEgress(opennsl_if_t egressId);
  const BcmEgressBase*  getEgressObjectIf(opennsl_if_t egress) const;
  BcmEgressBase* getEgressObjectIf(opennsl_if_t egress);

 private:
  const BcmSwitch* hw_;

  template<typename KeyT, typename HostT>
  using HostMap = boost::container::flat_map<
    KeyT, std::pair<std::unique_ptr<HostT>, uint32_t>>;

  typedef std::pair<opennsl_vrf_t, folly::IPAddress> Key;
  HostMap<Key, BcmHost> hosts_;
  typedef std::pair<opennsl_vrf_t, RouteForwardNexthops> EcmpKey;
  HostMap<EcmpKey, BcmEcmpHost> ecmpHosts_;

  template<typename KeyT, typename HostT, typename... Args>
  HostT* incRefOrCreateBcmHost(HostMap<KeyT, HostT>* map, const KeyT& key,
      Args... args);
  template<typename KeyT, typename HostT, typename... Args>
  HostT* getBcmHostIf(const HostMap<KeyT, HostT> *map, Args... args) const;
  template<typename KeyT, typename HostT, typename... Args>
  HostT* derefBcmHost(HostMap<KeyT, HostT>* map, Args... args) noexcept;

  boost::container::flat_map<opennsl_if_t,
    std::pair<std::unique_ptr<BcmEgressBase>, uint32_t>> egressMap_;
};

}}
