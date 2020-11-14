/**
 * @file rpc_utils.h
 *
 * @date Feb 3, 2017
 */

#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

#include "../derecho_exception.hpp"
#include "../derecho_type_definitions.hpp"
#include "derecho_internal.hpp"
#include <derecho/mutils-serialization/SerializationSupport.hpp>
#include <derecho/utils/logger.hpp>
#include <mutils/macro_utils.hpp>

namespace derecho {

namespace rpc {

/**
 * Computes a simple hash of a C-string (raw array of char) in a constexpr
 * function that can be run at compile-time. This allows us to generate
 * FunctionTags at compile time, inside template parameters, using the literal
 * names of functions (which can be converted to C-string constants using macro
 * stringization).
 */
template <typename Carr>
constexpr std::size_t hash_cstr(Carr&& c_str) {
    std::size_t hash_code = 0;
    for(char c : c_str) {
        if(c == 0) break;
        hash_code = hash_code * 31 + c;
    }
    return hash_code;
}

/**
 * A helper function that examines a C-string to determine whether it matches
 * one of the two method-registering macros in register_rpc_functions, P2P_TARGETS
 * and ORDERED_TARGETS. This is used by the REGISTER_RPC_FUNCTIONS macro to
 * ensure that it is only called with the correct arguments.
 */
template <typename Carr>
constexpr bool well_formed_macro(Carr&& c_str) {
    constexpr char option1[] = "P2P_TARGETS";
    constexpr std::size_t length1 = 11;
    constexpr char option2[] = "ORDERED_TARGETS";
    constexpr std::size_t length2 = 15;
    bool option1_active = false;
    bool option2_active = false;
    std::size_t index = 0;
    for(char c : c_str) {
        if(c == 0 || (index >= length1 && option1_active) || (index >= length2 && option2_active))
            return true;
        else if(!option1_active && !option2_active && index == 0) {
            //first iter
            if(c == option1[0])
                option1_active = true;
            else if(c == option2[0])
                option2_active = true;
            else
                return false;
            ++index;
        } else if(option1_active) {
            if(c != option1[index]) return false;
            ++index;
        } else if(option2_active) {
            if(c != option2[index]) return false;
            ++index;
        } else
            return false;
    }
    return true;
}

using FunctionTag = unsigned long long;

/**
 * Converts a user-supplied FunctionTag (from the result of hash_cstr) to an
 * "internal" FunctionTag, which uses its least-significant bit to indicate
 * whether it is an ordered function or a P2P function.
 * @tparam IsP2P true if this tag is for a P2P-callable function, false if it
 * is for an ordered function
 * @param user_tag The user-supplied FunctionTag based on the function's name
 * @return A FunctionTag that is even if the RPC function is ordered, and odd
 * if the RPC function is P2P-callable
 */
template<bool IsP2P>
constexpr FunctionTag to_internal_tag(FunctionTag user_tag) {
    if constexpr(IsP2P) {
        return 2 * user_tag + 1;
    } else {
        return 2 * user_tag;
    }
}

/*
template<>
constexpr FunctionTag to_internal_tag<true>(FunctionTag user_tag) {
    return 2 * user_tag + 1;
}

template<>
constexpr FunctionTag to_internal_tag<false>(FunctionTag user_tag) {
    return 2 * user_tag;
}
*/

/**
 * An RPC function call can be uniquely identified by the tuple
 * (class, subgroup ID, function ID, is-reply), which is what this struct
 * encapsulates. Its comparsion operators simply inherit the ones from
 * std::tuple.
 */
struct Opcode {
    subgroup_type_id_t class_id;
    subgroup_id_t subgroup_id;
    FunctionTag function_id;
    bool is_reply;
};
inline bool operator<(const Opcode& lhs, const Opcode& rhs) {
    return std::tie(lhs.class_id, lhs.subgroup_id, lhs.function_id, lhs.is_reply)
           < std::tie(rhs.class_id, rhs.subgroup_id, rhs.function_id, rhs.is_reply);
}
inline bool operator==(const Opcode& lhs, const Opcode& rhs) {
    return lhs.class_id == rhs.class_id && lhs.subgroup_id == rhs.subgroup_id
           && lhs.function_id == rhs.function_id && lhs.is_reply == rhs.is_reply;
}

using node_list_t = std::vector<node_id_t>;

/**
 * Indicates that an RPC call failed because executing the RPC function on the
 * remote node resulted in an exception.
 */
struct remote_exception_occurred : public derecho_exception {
    node_id_t who;
    remote_exception_occurred(node_id_t who)
            : derecho_exception(std::string("An exception occurred at node with ID ")
                                + std::to_string(who)),
              who(who) {}
};

/**
 * Indicates that an RPC call to a node failed because the node was removed
 * from the Replicated Object's subgroup (and possibly from the enclosing Group
 * entirely) after the RPC message was sent but before a reply was received.
 */
struct node_removed_from_group_exception : public derecho_exception {
    node_id_t who;
    node_removed_from_group_exception(node_id_t who)
            : derecho_exception(std::string("Node with ID ")
                                + std::to_string(who)
                                + std::string(" has been removed from the group.")),
              who(who) {}
};

/**
 * Indicates that an RPC call from this node was aborted because this node was
 * removed from its subgroup/shard (and reassigned to another one) during the
 * view change.
 */
struct sender_removed_from_group_exception : public derecho_exception {
    sender_removed_from_group_exception()
            : derecho_exception("This node was removed from its subgroup or shard "
                                "and can no longer send the RPC message.") {}
};

/**
 * Return type of all the RemoteInvocable::receive_* methods. If the method is
 * receive_call, this struct contains the message to send in reply, along with
 * its size in bytes, and a pointer to the exception generated by the function
 * call if one was thrown.
 */
struct recv_ret {
    Opcode opcode;
    std::size_t size;
    char* payload;
    std::exception_ptr possible_exception;
};

/**
 * Type signature for all the RemoteInvocable::receive_* methods. This alias is
 * helpful for declaring a map of "RPC receive handlers" that are called when
 * some RPC message is received.
 */
using receive_fun_t = std::function<recv_ret(
        //        mutils::DeserializationManager* dsm,
        mutils::RemoteDeserialization_v* rdv, const node_id_t&, const char* recv_buf,
        const std::function<char*(int)>& out_alloc)>;

/**
 * The type of map contained in a QueryResults::ReplyMap. The template parameter
 * should be the return type of the query.
 */
template <typename T>
using reply_map = std::map<node_id_t, std::future<T>>;

/**
 * Data structure that (indirectly) holds a set of futures for a single RPC
 * function call; there is one future for each node contacted to make the
 * call, and it will eventually contain that node's reply. The futures are
 * actually stored inside an internal struct of type ReplyMap, which can be
 * retrieved with the get() method. The ReplyMap will not be returned until
 * it is "fulfilled" by the sender, which should happen when the RPC call
 * is delivered in the current View (and thus, the current View is the set
 * of nodes who should reply to the RPC).
 * @tparam Ret The return type of the RPC function that this query invoked
 */
template <typename Ret>
class QueryResults {
public:
    using map_fut = std::future<std::unique_ptr<reply_map<Ret>>>;
    using map = reply_map<Ret>;
    using type = Ret;

    class ReplyMap {
    private:
        QueryResults& parent;

    public:
        map rmap;

        ReplyMap(QueryResults& qr) : parent(qr){};
        ReplyMap(const ReplyMap&) = delete;
        ReplyMap(ReplyMap&& rm) : parent(rm.parent), rmap(std::move(rm.rmap)) {}

        bool valid(const node_id_t& nid) {
            assert(rmap.size() == 0 || rmap.count(nid) != 0);
            return (rmap.size() > 0) && rmap.at(nid).valid();
        }

        /*
          returns true if we sent to this node,
          regardless of whether this node has replied.
        */
        bool contains(const node_id_t& nid) { return rmap.count(nid); }

        auto begin() { return std::begin(rmap); }

        auto end() { return std::end(rmap); }

        Ret get(const node_id_t& nid) {
            if(rmap.size() == 0) {
                assert(parent.pending_rmap.valid());
                rmap = std::move(*parent.pending_rmap.get());
            }
            assert(rmap.size() > 0);
            assert(rmap.count(nid));
            assert(rmap.at(nid).valid());
            return rmap.at(nid).get();
        }
    };

    map_fut pending_rmap;

private:
    ReplyMap replies{*this};

public:
    QueryResults(map_fut pm) : pending_rmap(std::move(pm)) {}
    QueryResults(QueryResults&& o)
            : pending_rmap{std::move(o.pending_rmap)},
              replies{std::move(o.replies)} {}
    QueryResults(const QueryResults&) = delete;

    /**
     * Wait the specified duration; if a ReplyMap is available
     * after that duration, return it. Otherwise return nullptr.
     */
    template <typename Time>
    ReplyMap* wait(Time t) {
        if(replies.rmap.size() == 0) {
            if(pending_rmap.wait_for(t) == std::future_status::ready) {
                replies.rmap = std::move(*pending_rmap.get());
                return &replies;
            } else
                return nullptr;
        } else
            return &replies;
    }

    /**
     * Block until the ReplyMap is fulfilled, then return the map by reference.
     * The ReplyMap is only valid as long as this QueryResults remains in
     * scope, and cannot be copied.
     */
    ReplyMap& get() {
        using namespace std::chrono;
        while(true) {
            if(auto rmap = wait(5min)) {
                return *rmap;
            }
        }
    }
};

/**
 * Specialization of QueryResults for void functions, which do not generate
 * replies. Here the "reply map" is actually a set, and simply records the set
 * of nodes to which the RPC was sent. The internal ReplyMap is fulfilled when
 * the set of nodes that received the RPC is known, which is when the RPC
 * message was delivered in the current View.
 */
template <>
class QueryResults<void> {
public:
    using map_fut = std::future<std::unique_ptr<std::set<node_id_t>>>;
    using map = std::set<node_id_t>;
    using type = void;

    class ReplyMap {
    private:
        QueryResults& parent;

    public:
        map rmap;

        ReplyMap(QueryResults& qr) : parent(qr){};
        ReplyMap(const ReplyMap&) = delete;
        ReplyMap(ReplyMap&& rm) : parent(rm.parent), rmap(std::move(rm.rmap)) {}

        bool valid(const node_id_t& nid) {
            assert(rmap.size() == 0 || rmap.count(nid) != 0);
            return (rmap.size() > 0) && rmap.count(nid) > 0;
        }

        /*
          returns true if we sent to this node,
          regardless of whether this node has replied.
        */
        bool contains(const node_id_t& nid) { return rmap.count(nid); }

        auto begin() { return std::begin(rmap); }

        auto end() { return std::end(rmap); }
    };

    map_fut pending_rmap;

private:
    ReplyMap replies{*this};

public:
    QueryResults(map_fut pm) : pending_rmap(std::move(pm)) {}
    QueryResults(QueryResults&& o)
            : pending_rmap{std::move(o.pending_rmap)},
              replies{std::move(o.replies)} {}
    QueryResults(const QueryResults&) = delete;

    /**
     * Wait the specified duration; if a ReplyMap is available
     * after that duration, return it. Otherwise return nullptr.
     */
    template <typename Time>
    ReplyMap* wait(Time t) {
        if(replies.rmap.size() == 0) {
            if(pending_rmap.wait_for(t) == std::future_status::ready) {
                replies.rmap = std::move(*pending_rmap.get());
                return &replies;
            } else
                return nullptr;
        } else
            return &replies;
    }

    /**
     * Block until the ReplyMap is fulfilled, then return the map by reference.
     * The ReplyMap is only valid as long as this QueryResults remains in
     * scope, and cannot be copied.
     */
    ReplyMap& get() {
        using namespace std::chrono;
        while(true) {
            if(auto rmap = wait(5min)) {
                return *rmap;
            }
        }
    }
};

/**
 * Abstract base type for PendingResults. This allows us to store a pointer to
 * any template specialization of PendingResults without knowing the template
 * parameter.
 */
class PendingBase {
public:
    virtual void fulfill_map(const node_list_t&) = 0;
    virtual void set_exception_for_removed_node(const node_id_t&) = 0;
    virtual void set_exception_for_caller_removed() = 0;
    virtual bool all_responded() const = 0;
    virtual void reset() = 0;
    virtual ~PendingBase() {}
};

/**
 * Data structure that holds a set of promises for a single RPC function call;
 * the promises transmit one response (either a value or an exception) for
 * each node that was called. The future ends of these promises are stored in
 * a corresponding QueryResults object.
 * @tparam Ret The return type of the RPC function, which is the type of a
 * response's value.
 */
template <typename Ret>
class PendingResults : public PendingBase {
private:
    /** A promise for a map containing one future for each reply to the RPC function
     * call. The future end of this promise lives in QueryResults, and is fulfilled
     * when the RPC function call is actually sent and the set of repliers is known. */
    std::promise<std::unique_ptr<reply_map<Ret>>> promise_for_pending_map;

    std::promise<std::map<node_id_t, std::promise<Ret>>> promise_for_reply_promises;
    /** A future for a map containing one promise for each reply to the RPC function
     * call. It will be fulfilled when fulfill_map is called, which means the RPC
     * function call was actually sent and the set of destination nodes is known. */
    std::future<std::map<node_id_t, std::promise<Ret>>> reply_promises_are_ready;
    std::mutex reply_promises_are_ready_mutex;
    std::map<node_id_t, std::promise<Ret>> reply_promises;

    bool map_fulfilled = false;
    std::set<node_id_t> dest_nodes, responded_nodes;

public:
    PendingResults()
            : reply_promises_are_ready(promise_for_reply_promises.get_future()) {}
    virtual ~PendingResults() {}

    /**
     * Constructs and returns a QueryResults representing the "future" end of
     * the response promises in this PendingResults.
     * @return A new QueryResults holding a set of futures for this RPC function call
     */
    QueryResults<Ret> get_future() {
        return QueryResults<Ret>{promise_for_pending_map.get_future()};
    }

    /**
     * Fill pending_map and reply_promises with one promise/future pair for
     * each node that was contacted in this RPC call
     * @param who A list of nodes from which to expect responses.
     */
    void fulfill_map(const node_list_t& who) {
        dbg_default_trace("Got a call to fulfill_map for PendingResults<{}>", typeid(Ret).name());
        std::unique_ptr<reply_map<Ret>> futures_map = std::make_unique<reply_map<Ret>>();
        std::map<node_id_t, std::promise<Ret>> promises_map;
        for(const auto& e : who) {
            futures_map->emplace(e, promises_map[e].get_future());
        }
        dest_nodes.insert(who.begin(), who.end());
        dbg_default_trace("Setting a value for reply_promises_are_ready");
        promise_for_reply_promises.set_value(std::move(promises_map));
        promise_for_pending_map.set_value(std::move(futures_map));
        map_fulfilled = true;
    }

    /**
     * Sets exceptions to indicate to the sender of this RPC call that it has been
     * removed from its subgroup/shard, and can no longer expect responses.
     */
    void set_exception_for_caller_removed() {
        if(!map_fulfilled) {
            promise_for_pending_map.set_exception(
                    std::make_exception_ptr(sender_removed_from_group_exception{}));
        } else {
            if(reply_promises.size() == 0) {
                reply_promises = std::move(reply_promises_are_ready.get());
            }
            //Set exceptions for any nodes that have not yet responded
            for(auto& node_and_promise : reply_promises) {
                if(responded_nodes.find(node_and_promise.first)
                   == responded_nodes.end()) {
                    node_and_promise.second.set_exception(
                            std::make_exception_ptr(sender_removed_from_group_exception{}));
                }
            }
        }
    }

    void set_exception_for_removed_node(const node_id_t& removed_nid) {
        assert(map_fulfilled);
        if(dest_nodes.find(removed_nid) != dest_nodes.end()
           && responded_nodes.find(removed_nid) == responded_nodes.end()) {
            set_exception(removed_nid,
                          std::make_exception_ptr(
                                  node_removed_from_group_exception{removed_nid}));
        }
    }

    /**
     * Fulfills a promise for a single node's reply by setting the value that
     * the node returned for the RPC call
     * @param nid The node that responded to the RPC call
     * @param v The value that it returned as the result of the RPC function
     */
    void set_value(const node_id_t& nid, const Ret& v) {
        std::lock_guard<std::mutex> lock(reply_promises_are_ready_mutex);
        responded_nodes.insert(nid);
        if(reply_promises.size() == 0) {
            dbg_default_trace("PendingResults<{}>::set_value about to wait on reply_promises_are_ready", typeid(Ret).name());
            dbg_default_flush();
            reply_promises = std::move(reply_promises_are_ready.get());
        }
        reply_promises.at(nid).set_value(v);
    }

    /**
     * Fulfills a promise for a single node's reply by setting an exception that
     * was thrown by the RPC function call.
     * @param nid The node that responded to the RPC call with an exception
     * @param e The exception_ptr that the RPC function call returned
     */
    void set_exception(const node_id_t& nid, const std::exception_ptr e) {
        responded_nodes.insert(nid);
        if(reply_promises.size() == 0) {
            reply_promises = std::move(reply_promises_are_ready.get());
        }
        reply_promises.at(nid).set_exception(e);
    }

    /**
     * @return True if all destination nodes for this RPC function call have
     * responded, either by sending a reply or by being removed from the group
     */
    bool all_responded() const {
        return map_fulfilled && (responded_nodes == dest_nodes);
    }

    /**
     * reset this object.
     */
    void reset() {
        promise_for_pending_map = std::promise<std::unique_ptr<reply_map<Ret>>>();
        promise_for_reply_promises = std::promise<std::map<node_id_t, std::promise<Ret>>>();
        reply_promises_are_ready = promise_for_reply_promises.get_future();
        // reply_promises_are_ready_mutex
        reply_promises.clear();
        map_fulfilled = false;
        dest_nodes.clear();
        responded_nodes.clear();
    }
};

/**
 * Specialization of PendingResults for void functions, which do not generate
 * replies. Its only functionality is to fulfill the "reply map" in its
 * corresponding QueryResults<void>, which is just a set of nodes to which the
 * RPC message was delivered.
 */
template <>
class PendingResults<void> : public PendingBase {
private:
    std::promise<std::unique_ptr<std::set<node_id_t>>> promise_for_pending_map;
    bool map_fulfilled = false;

public:
    QueryResults<void> get_future() {
        return QueryResults<void>(promise_for_pending_map.get_future());
    }

    void fulfill_map(const node_list_t& sent_nodes) {
        auto nodes_sent_set = std::make_unique<std::set<node_id_t>>();
        for(const node_id_t& node : sent_nodes) {
            nodes_sent_set->emplace(node);
        }
        promise_for_pending_map.set_value(std::move(nodes_sent_set));
        map_fulfilled = true;
    }

    void set_exception_for_removed_node(const node_id_t&) {}

    void set_exception_for_caller_removed() {
        if(!map_fulfilled) {
            promise_for_pending_map.set_exception(
                    std::make_exception_ptr(sender_removed_from_group_exception()));
        }
    }

    bool all_responded() const {
        return map_fulfilled;
    }

    void reset() {
        promise_for_pending_map = std::promise<std::unique_ptr<std::set<node_id_t>>>();
        map_fulfilled = false;
    }
};

/**
 * Utility functions for manipulating the headers of RPC messages
 */
namespace remote_invocation_utilities {
#define RPC_HEADER_FLAG_TST(f, name) \
    ((f) & (((uint32_t)1L) << (_RPC_HEADER_FLAG_##name)))
#define RPC_HEADER_FLAG_SET(f, name) \
    ((f) |= (((uint32_t)1L) << (_RPC_HEADER_FLAG_##name)))
#define RPC_HEADER_FLAG_CLR(f, name) \
    ((f) &= ~(((uint32_t)1L) << (_RPC_HEADER_FLAG_##name)))

// add new rpc header flags here.
#define _RPC_HEADER_FLAG_CASCADE (0)
#define _RPC_HEADER_FLAG_RESERVED (1)

inline std::size_t header_space() {
    return sizeof(std::size_t) + sizeof(Opcode) + sizeof(node_id_t) + sizeof(uint32_t);
    //            size                  operation        from                flags
}

inline char* extra_alloc(int i) {
    const auto hs = header_space();
    return (char*)calloc(i + hs, sizeof(char)) + hs;
}

inline void populate_header(char* reply_buf,
                            const std::size_t& payload_size,
                            const Opcode& op, const node_id_t& from,
                            const uint32_t& flags) {
    std::size_t offset = 0;
    static_assert(sizeof(op) == sizeof(Opcode), "Opcode& is not the same size as Opcode!");
    reinterpret_cast<std::size_t*>(reply_buf + offset)[0] = payload_size;  // size
    offset += sizeof(payload_size);
    reinterpret_cast<Opcode*>(reply_buf + offset)[0] = op;  // what
    offset += sizeof(op);
    reinterpret_cast<node_id_t*>(reply_buf + offset)[0] = from;  // from
    offset += sizeof(from);
    reinterpret_cast<uint32_t*>(reply_buf + offset)[0] = flags;  // flags
}

//inline void retrieve_header(mutils::DeserializationManager* dsm,
inline void retrieve_header(mutils::RemoteDeserialization_v* rdv,
                            const char* reply_buf,
                            std::size_t& payload_size, Opcode& op,
                            node_id_t& from, uint32_t& flags) {
    std::size_t offset = 0;
    payload_size = reinterpret_cast<const std::size_t*>(reply_buf + offset)[0];
    offset += sizeof(payload_size);
    op = reinterpret_cast<const Opcode*>(reply_buf + offset)[0];
    offset += sizeof(op);
    from = reinterpret_cast<const node_id_t*>(reply_buf + offset)[0];
    offset += sizeof(from);
    flags = reinterpret_cast<const uint32_t*>(reply_buf + offset)[0];
}
}  // namespace remote_invocation_utilities

}  // namespace rpc
}  // namespace derecho

#define CT_STRING(...) derecho::rpc::String<MACRO_GET_STR(#__VA_ARGS__)>
