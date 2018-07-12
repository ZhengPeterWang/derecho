/**
 * @file api_cost.cpp
 *
 * @date June 26, 2017
 */

#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "derecho/derecho.h"
#include "derecho/multicast_group.h"
#include "initialize.h"
#include "log_results.h"
#include "rdmc/util.h"
#include "test_objects.h"

using derecho::ExternalCaller;
using derecho::Replicated;
using std::cout;
using std::endl;

struct exp_result {
    uint32_t num_nodes;
    long long unsigned int max_msg_size;
    unsigned int window_size;
    int num_messages;
    int raw_mode;
    double latency;

    void print(std::ofstream& fout) {
        fout << num_nodes << " " << max_msg_size
             << " " << window_size << " "
             << num_messages << " "
             << raw_mode << " " << latency << endl;
    }
};

constexpr auto num_messages = 1000u;
volatile bool done = false;

//probably paying attention to node 0's ones of these.
std::vector<uint64_t> start_times(num_messages), end_times(num_messages);
constexpr auto num_nodes = 3u;

decltype(auto) stability_callback(int32_t subgroup, int sender_id, long long int index, char* buf, long long int msg_size) {
    // cout << buf << endl;
    // cout << "Delivered a message" << endl;
    DERECHO_LOG(sender_id, index, "complete_send");
    if(sender_id == 0) {
        end_times[index] = get_time();
    }
    whendebug(std::cout << "index is " << index << std::endl);
    if(index == num_messages - 1) {
        whendebug(std::cout << "we are done" << std::endl);
        done = true;
    }
};

struct Faz {
    static constexpr std::size_t test_array_size = 131072;

    std::array<std::size_t, test_array_size> state;

    std::array<std::size_t, test_array_size> read_state() {
        whendebug(std::cout << std::endl
                            << "executing read_state" << std::endl
                            << std::endl);
        return state;
    }
    void change_state(std::array<std::size_t, test_array_size> new_state) {
        whendebug(std::cout << std::endl
                            << "executing change_state " << new_state[0] << std::endl
                            << std::endl);
        end_times[new_state[0]] = get_time();
        if(new_state[0] == (num_messages - 1)) {
            done = true;
            whendebug(std::cout << "we are done" << std::endl;);
        }
    }

    REGISTER_RPC_FUNCTIONS(Faz, read_state, change_state);

    /**
     * Constructs a Faz with an initial value.
     * @param initial_state
     */
    Faz() = default;
    Faz(const Faz&) = default;
};

static_assert(std::is_standard_layout<Faz>::value, "Erorr: Faz not standard layout");
static_assert(std::is_pod<Faz>::value, "Erorr: Faz not POD");
static_assert(sizeof(Faz) == sizeof(std::size_t) * Faz::test_array_size, "Error: RTTI?");

int main(int argc, char** argv) {
    using namespace std;
    derecho::node_id_t node_id;
    derecho::ip_addr my_ip;
    derecho::ip_addr leader_ip;

    query_node_info(node_id, my_ip, leader_ip);
    {
        using namespace chrono;
        this_thread::sleep_for(seconds{node_id});
    }

    //Derecho message parameters
    //Where do these come from? What do they mean? Does the user really need to supply them?
    long long unsigned int max_msg_size = Faz::test_array_size * sizeof(std::size_t) + 100;
    long long unsigned int block_size = 100000;
    constexpr auto window_size = 3u;
    constexpr auto raw_mode = false;
    derecho::DerechoParams derecho_params{max_msg_size, block_size};

    //note: stability callback isn't going to happen in cooked mode.
    derecho::CallbackSet callback_set{derecho::message_callback_t{stability_callback}, {}};

    //Since this is just a test, assume there will always be 3 members with IDs 0-2
    //Assign Faz and Bar to a subgroup containing 0, 1, and 2
    derecho::SubgroupInfo subgroup_info{
            {{std::type_index(typeid(Faz)), [](const derecho::View& curr_view, int& next_unassigned_rank, bool previous_was_successful) {
                  if(curr_view.num_members < 3) {
                      std::cout << "Faz function throwing subgroup_provisioning_exception" << std::endl;
                      throw derecho::subgroup_provisioning_exception();
                  }
                  derecho::subgroup_shard_layout_t subgroup_vector(1);
                  std::vector<derecho::node_id_t> first_3_nodes(&curr_view.members[0], &curr_view.members[0] + 3);
                  //Put the desired SubView at subgroup_vector[0][0] since there's one subgroup with one shard
                  subgroup_vector[0].emplace_back(curr_view.make_subview(first_3_nodes, derecho::Mode::ORDERED, {false, true, false}));
                  next_unassigned_rank = std::max(next_unassigned_rank, 3);
                  return subgroup_vector;
              }}},
            {std::type_index(typeid(Faz))}};

    //Each replicated type needs a factory; this can be used to supply constructor arguments
    //for the subgroup's initial state
    auto faz_factory = [](PersistentRegistry*) { return std::make_unique<Faz>(); };

    std::unique_ptr<derecho::Group<Faz>> group;
    if(my_ip == leader_ip) {
        group = std::make_unique<derecho::Group<Faz>>(
                node_id, my_ip, callback_set, subgroup_info, derecho_params,
                std::vector<derecho::view_upcall_t>{}, derecho::derecho_gms_port,
                faz_factory);
    } else {
        group = std::make_unique<derecho::Group<Faz>>(
                node_id, my_ip, leader_ip, callback_set, subgroup_info,
                std::vector<derecho::view_upcall_t>{}, derecho::derecho_gms_port,
                faz_factory);
    }

    cout << "Finished constructing/joining Group" << endl;

    vector<uint32_t> members;
    for(uint32_t i = 0; i < num_nodes; i++) members.push_back(i);
    unique_ptr<rdmc::barrier_group> universal_barrier_group = std::make_unique<rdmc::barrier_group>(members);

    universal_barrier_group->barrier_wait();
    uint64_t t1 = get_time();
    universal_barrier_group->barrier_wait();
    uint64_t t2 = get_time();
    reset_epoch();
    universal_barrier_group->barrier_wait();
    uint64_t t3 = get_time();
    printf(
            "Synchronized clocks.\nTotal possible variation = %5.3f us\n"
            "Max possible variation from local = %5.3f us\n",
            (t3 - t1) * 1e-3f, max(t2 - t1, t3 - t2) * 1e-3f);
    fflush(stdout);

    if(node_id == 0) {
        //Replicated<Faz>& faz_rpc_handle = group->get_subgroup<Faz>();
        //faz_rpc_handle.get_sendbuffer_ptr();
        //faz_rpc_handle.ordered_query<RPC_NAME(read_state)>();
    }
    if(node_id == 1) {
        Replicated<Faz>& faz_rpc_handle = group->get_subgroup<Faz>();
        for(auto i = 0u; i < num_messages; ++i) {
            DECT(Faz{}.state)
            new_value = {i};
            whendebug(cout << "Changing Faz's state round " << i << endl);
            start_times[i] = get_time();
            /*derecho::rpc::QueryResults<bool> results = */ faz_rpc_handle.ordered_send<RPC_NAME(change_state)>(new_value);
            whendebug(std::cout << "checkpoint: query issued" << std::endl);
            //don't wait for replies, because there is no equivalent in uncooked mode.
            /*
          decltype(results)::ReplyMap& replies = results.get();
          whendebug(cout << "Got a reply map!" << endl);
          for(auto& reply_pair : replies) {
            whendebug(cout << "Reply from node " << reply_pair.first << " was " << std::boolalpha << reply_pair.second.get() << endl);
            //block for replies even if we're not printing them.
            whenrelease(reply_pair.second.get());
          }
           //*/
        }
    }
    if(node_id == 2) { /*
        Replicated<Faz>& faz_rpc_handle = group->get_subgroup<Faz>();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        cout << "Reading Faz's state from the group" << endl;
        auto faz_results = faz_rpc_handle.ordered_query<RPC_NAME(read_state)>();
        for(auto& reply_pair : faz_results.get()) {
            cout << "Node " << reply_pair.first << " says the state is: " << reply_pair.second.get() << endl;
        }*/
    }
    cout << "Reached end of main(), loop on done" << std::endl;
    while(!done) {
    }
    cout << "Exit done loop" << std::endl;
    group->barrier_sync();
    this_thread::sleep_for(chrono::seconds{3});
    uint64_t total_time = 0;
    for(auto i = 0u; i < num_messages; ++i) {
        total_time += end_times[i] - start_times[i];
    }
    if(node_id == 1) {
        log_results(exp_result{num_nodes, max_msg_size, window_size, num_messages, raw_mode, ((double)total_time) / (num_messages * 1000)}, "data_latency");
    }
}