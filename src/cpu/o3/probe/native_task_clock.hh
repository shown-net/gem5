// Read-only mirror of BaseCPU's existing cycle probe contract. No target inputs.
#ifndef __CPU_O3_PROBE_NATIVE_TASK_CLOCK_HH__
#define __CPU_O3_PROBE_NATIVE_TASK_CLOCK_HH__
#include <cstdint>
#include <limits>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace gem5 { namespace o3 { namespace task_clock {
inline constexpr const char *Contract="task-lifecycle-basecpu-active-v1";
inline constexpr const char *CounterDomain="gem5-basecpu-active-cycles-v1";
enum class State { Sleep=0, Wakeup=1, On=2 };
struct Update {
 uint64_t first_tick=0,last_tick=0,first_cycle=0,last_cycle=0,count=1;
 uint64_t delta=0,active_delta=0,period=0,first_active_counter=0,last_active_counter=0;
 uint64_t first_sequence=0,last_sequence=0;
 bool bootstrap=false;int sleeping=-1;State previous_state=State::Sleep,next_state=State::Sleep;
};
struct Snapshot {
 std::string kind;uint64_t record_ordinal=0,tick=0,cycle=0,active_counter=0,sequence=0;
 int32_t pid=0;bool enabled=false,known=false;
};
struct Interval {
 uint64_t start_tick=0,end_tick=0,start_cycle=0,end_cycle=0,active_cycles=0;
 uint64_t start_sequence=0,end_sequence=0;
 int32_t pid=0;bool enabled=false,known=false;
};
class Ledger {
 public:
 // Rebase an observer-local counter from the CPU's existing public state.
 // No CPU update is requested, including when an idle CPU has no callbacks.
 void bootstrap(uint64_t tick,uint64_t cycle,uint64_t period,uint64_t previous,State state) {
  if(observed_||have_active_||have_sleeping_){reject("duplicate-native-counter-epoch");return;}
  check_period(period);
  if(previous>cycle||int(state)<0||int(state)>2)reject("invalid-native-counter-epoch");
  epoch_=true;epoch_tick_=tick;epoch_cycle_=cycle;epoch_previous_=previous;epoch_state_=state;
  state_=state;last_cycle_=previous;last_tick_=tick;observed_=true;sequence_=1;
 }
 bool observed()const{return observed_;}
 void active(uint64_t delta) {
  if(have_active_||have_sleeping_)reject("native-active-notification-order");
  have_active_=true;pending_active_=delta;
 }
 void sleeping(bool value) {
  if(have_sleeping_)reject("native-duplicate-sleeping-notification");
  have_sleeping_=true;pending_sleeping_=value?1:0;
 }
 void cycles(uint64_t delta,uint64_t tick,uint64_t cycle,uint64_t period) {
  check_period(period);
  if(observed_&&(cycle<last_cycle_||
      (state_==State::On&&cycle-last_cycle_>UINT32_MAX)||
      delta!=uint32_t(cycle-last_cycle_)))
   reject("native-counter-delta-or-overflow");
  if(delta>UINT32_MAX)reject("native-counter-delta-or-overflow");
  if(have_active_!=(state_==State::On)||
     (have_active_&&pending_active_!=delta))reject("native-active-delta-mismatch");
  const auto before=counter_;
  if(have_active_)add(counter_,pending_active_);
  Update u;u.first_tick=u.last_tick=tick;u.first_cycle=u.last_cycle=cycle;
  u.bootstrap=!observed_;u.delta=delta;u.active_delta=have_active_?pending_active_:0;u.period=period;
  u.first_active_counter=before;u.last_active_counter=counter_;
  u.sleeping=have_sleeping_?pending_sleeping_:-1;u.previous_state=state_;
  u.next_state=u.sleeping==1?State::Sleep:u.sleeping==0?State::Wakeup:State::On;
  u.first_sequence=u.last_sequence=sequence_++;
  const bool regular=u.sleeping==-1&&u.previous_state==State::On&&u.next_state==State::On&&delta==1;
  if(!break_run_&&regular&&!updates_.empty()) {
   auto &p=updates_.back();
   if(p.sleeping==-1&&p.previous_state==State::On&&p.next_state==State::On&&p.delta==1&&
      p.period==period&&p.last_cycle+1==cycle&&p.last_tick+period==tick&&
      p.last_sequence+1==u.first_sequence) {
    ++p.count;p.last_tick=tick;p.last_cycle=cycle;p.last_sequence=u.last_sequence;
    p.last_active_counter=counter_;
   } else updates_.push_back(u);
  } else updates_.push_back(u);
  break_run_=false;
  if(u.sleeping==1&&measuring_&&known_&&enabled_)halted_pids_.insert(pid_);
  if(u.sleeping==1&&measuring_&&!known_&&!saw_cut_)unresolved_initial_halt_=true;
  state_=u.next_state;last_cycle_=cycle;last_tick_=tick;observed_=true;
  have_active_=have_sleeping_=false;pending_active_=0;pending_sleeping_=-1;
 }
 void initial(int32_t pid,bool known) {
  if(!owner_initialized_){pid_=pid;known_=known;enabled_=known;owner_initialized_=true;
   initial_pid_=pid;initial_known_=known;initial_enabled_=known;}
 }
 void begin(uint64_t tick,uint64_t cycle,uint64_t period,uint64_t ordinal) {
  if(measuring_||closed_)reject("duplicate-native-roi-begin");
  measuring_=true;
  if(!known_&&state_!=State::On)unresolved_initial_halt_=true;
  if(known_&&enabled_&&state_!=State::On)halted_pids_.insert(pid_);
  snapshot("roi_begin",tick,cycle,period,ordinal);
 }
 void out(int32_t prev,int32_t next,uint64_t tick,uint64_t cycle,uint64_t period,uint64_t ordinal,
          bool resolve_initial_prefix=false) {
  // Keep the raw unknown observations. The first retained out is a separate
  // functional witness; a transition at source start would first require in.
  if(resolve_initial_prefix&&!saw_cut_&&owner_initialized_&&!initial_known_&&prev==initial_pid_&&
     (prev==0||!unresolved_initial_halt_)) {
   initial_resolution_=true;initial_resolution_ordinal_=ordinal;initial_resolution_sequence_=sequence_;
   if(unresolved_initial_macros_)macros_[initial_pid_]+=unresolved_initial_macros_;
  }
  saw_cut_=true;
  if(prev==next||transition_||(known_&&(!enabled_||pid_!=prev)))reject("native-account-out-order");
  pending_prev_=prev;pending_next_=next;transition_=true;pid_=prev;known_=true;enabled_=false;
  snapshot("account_out",tick,cycle,period,ordinal);
 }
 void in(int32_t prev,int32_t next,uint64_t tick,uint64_t cycle,uint64_t period,uint64_t ordinal) {
  saw_cut_=true;
  if(!transition_||pending_prev_!=prev||pending_next_!=next||enabled_)
   reject("native-account-in-order");
  pid_=next;known_=true;enabled_=true;transition_=false;
  snapshot("account_in",tick,cycle,period,ordinal);
 }
 void end(uint64_t tick,uint64_t cycle,uint64_t period,uint64_t ordinal) {
  if(!measuring_||closed_)reject("native-roi-end-order");
  snapshot("roi_end",tick,cycle,period,ordinal);closed_=true;measuring_=false;
 }
 // Exactly the accepted retired macro population, excluding hardware entries.
 void macro(bool completes,bool pseudo) {
  if(measuring_&&!closed_&&completes&&!pseudo) {
   if(known_&&enabled_)++macros_[pid_];
   else if(!known_&&!saw_cut_&&owner_initialized_)++unresolved_initial_macros_;
  }
 }
 uint64_t task_macros(int32_t pid)const {auto p=macros_.find(pid);return p==macros_.end()?0:p->second;}
 bool valid()const{return errors_.empty();}
 bool closed()const{return closed_;}
 bool known()const{return known_;}bool enabled()const{return enabled_;}int32_t pid()const{return pid_;}
 bool initial_known()const{return initial_known_;}bool initial_enabled()const{return initial_enabled_;}
 int32_t initial_pid()const{return initial_pid_;}
 bool initial_resolution()const{return initial_resolution_;}
 uint64_t initial_resolution_ordinal()const{return initial_resolution_ordinal_;}
 bool resolved_prefix(const Interval &span)const {
  return initial_resolution_&&!span.known&&span.pid==initial_pid_&&span.end_sequence<=initial_resolution_sequence_;
 }
 const auto& snapshots()const{return snapshots_;}const auto& intervals()const{return intervals_;}
 const auto& updates()const{return updates_;}const auto& halted_pids()const{return halted_pids_;}
 const auto& errors()const{return errors_;}
 void reject(const std::string &reason){errors_.insert(reason);}
 void write_json(std::ostream &o)const {
  if(epoch_)o<<"\"counter_epoch\":{\"schema\":\"gem5-basecpu-counter-epoch-v1\","
    <<"\"source\":\"BaseCPU::previousState/previousCycle\",\"sequence\":0,\"tick\":"<<epoch_tick_
    <<",\"cycle\":"<<epoch_cycle_<<",\"previous_cycle\":"<<epoch_previous_
    <<",\"previous_state\":"<<int(epoch_state_)<<",\"period\":"<<period_<<"},";
  o<<"\"updates\":[";bool comma=false;
  for(const auto &u:updates_){if(comma)o<<',';comma=true;
   o<<"{\"first_tick\":"<<u.first_tick<<",\"last_tick\":"<<u.last_tick
    <<",\"first_cycle\":"<<u.first_cycle<<",\"last_cycle\":"<<u.last_cycle
    <<",\"count\":"<<u.count<<",\"delta\":"<<u.delta<<",\"active_delta\":"<<u.active_delta
    <<",\"period\":"<<u.period<<",\"first_active_counter\":"<<u.first_active_counter
    <<",\"last_active_counter\":"<<u.last_active_counter<<",\"first_sequence\":"<<u.first_sequence
    <<",\"last_sequence\":"<<u.last_sequence<<",\"sleeping\":"<<u.sleeping
    <<",\"bootstrap\":"<<(u.bootstrap?"true":"false")<<",\"previous_state\":"<<int(u.previous_state)<<",\"next_state\":"<<int(u.next_state)<<'}';}
  o<<"],\"snapshots\":[";comma=false;
  for(const auto&s:snapshots_){if(comma)o<<',';comma=true;
   o<<"{\"kind\":\""<<s.kind<<"\",\"record_ordinal\":"<<s.record_ordinal
    <<",\"tick\":"<<s.tick<<",\"cycle\":"<<s.cycle<<",\"active_counter\":"<<s.active_counter
    <<",\"sequence\":"<<s.sequence<<",\"pid\":"<<s.pid<<",\"enabled\":"<<(s.enabled?"true":"false")
    <<",\"known\":"<<(s.known?"true":"false")<<'}';}
  o<<"],\"intervals\":[";comma=false;
  for(const auto&i:intervals_){if(comma)o<<',';comma=true;
   o<<"{\"start_tick\":"<<i.start_tick<<",\"end_tick\":"<<i.end_tick
    <<",\"start_cycle\":"<<i.start_cycle<<",\"end_cycle\":"<<i.end_cycle
    <<",\"active_cycles\":"<<i.active_cycles<<",\"start_sequence\":"<<i.start_sequence
    <<",\"end_sequence\":"<<i.end_sequence<<",\"pid\":"<<i.pid
    <<",\"enabled\":"<<(i.enabled?"true":"false")<<",\"known\":"<<(i.known?"true":"false")<<'}';}
  o<<"],\"halted_pids\":[";comma=false;for(auto p:halted_pids_){if(comma)o<<',';comma=true;o<<p;}
  o<<"],\"per_pid_macros\":[";comma=false;for(auto p:macros_){if(comma)o<<',';comma=true;
   o<<"{\"pid\":"<<p.first<<",\"macro_instructions\":"<<p.second<<'}';}
  o<<"],\"rejections\":[";comma=false;for(const auto&e:errors_){if(comma)o<<',';comma=true;o<<'"'<<e<<'"';}o<<']';
 }
 private:
 void add(uint64_t &a,uint64_t d){if(d>UINT64_MAX-a)reject("native-counter-overflow");else a+=d;}
 void check_period(uint64_t p){if(!p||(period_&&p!=period_))reject("native-frequency-change");if(!period_)period_=p;}
 void snapshot(const char*kind,uint64_t tick,uint64_t cycle,uint64_t period,uint64_t ordinal){
  check_period(period);if(!observed_)reject("native-clock-unobserved");
  if(have_active_||have_sleeping_)reject("native-incomplete-counter-notification");
  if(cycle<last_cycle_||tick<last_tick_||
     (state_==State::On&&cycle-last_cycle_>UINT32_MAX))reject("native-snapshot-counter-order");
  uint64_t total=counter_;if(state_==State::On&&cycle>=last_cycle_)add(total,cycle-last_cycle_);
  Snapshot s;s.kind=kind;s.record_ordinal=ordinal;s.tick=tick;s.cycle=cycle;s.active_counter=total;
  s.sequence=sequence_++;s.pid=pid_;s.enabled=enabled_;s.known=known_;
  if(measuring_&&s.kind!="roi_begin"&&!snapshots_.empty()){
   const auto&p=snapshots_.back();
   if(p.kind=="roi_begin"||p.kind=="account_out"||p.kind=="account_in"){
    if(tick<p.tick||cycle<p.cycle||total<p.active_counter)reject("native-snapshot-regression");
    intervals_.push_back({p.tick,tick,p.cycle,cycle,total-p.active_counter,p.sequence,s.sequence,p.pid,p.enabled,p.known});
   }
  }
  snapshots_.push_back(s);break_run_=true;
 }
 State state_=State::Sleep;uint64_t last_cycle_=0,last_tick_=0,counter_=0,period_=0,sequence_=0,pending_active_=0;
 bool epoch_=false;uint64_t epoch_tick_=0,epoch_cycle_=0,epoch_previous_=0;State epoch_state_=State::Sleep;
 int pending_sleeping_=-1;bool have_active_=false,have_sleeping_=false,observed_=false,break_run_=true;
 bool measuring_=false,closed_=false,known_=false,enabled_=false,owner_initialized_=false,transition_=false;
 bool initial_known_=false,initial_enabled_=false;int32_t initial_pid_=0,pid_=0,pending_prev_=0,pending_next_=0;
 bool saw_cut_=false,initial_resolution_=false,unresolved_initial_halt_=false;
 uint64_t initial_resolution_ordinal_=0,initial_resolution_sequence_=0,unresolved_initial_macros_=0;
 std::vector<Update>updates_;std::vector<Snapshot>snapshots_;std::vector<Interval>intervals_;
 std::map<int32_t,uint64_t>macros_;std::set<int32_t>halted_pids_;std::set<std::string>errors_;
};
}}}
#endif
