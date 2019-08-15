
#ifndef COMM_NET_FACTORY_H
#define COMM_NET_FACTORY_H

#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DebugLoc.h"

#include "morpheus/Utils.hpp"
#include "morpheus/ADT/CommunicationNet.hpp"

#include <functional>

namespace cn {
using namespace llvm;


// ------------------------------------------------------------------------------
// EmptyCN

struct EmptyCN final : public PluginCNBase {

  ~EmptyCN() = default;

  EmptyCN(const CallSite &cs) : call_name(cs.getCalledFunction()->getName()) {
    entry_place().name += call_name;
    exit_place().name += call_name;
    add_cf_edge(entry_place(), exit_place());
  }
  EmptyCN(const EmptyCN &) = delete;
  EmptyCN(EmptyCN &&) = default;

  void connect(AddressableCN &) {
    // no connection
  }

  string call_name;
};


// ------------------------------------------------------------------------------
// CN_MPI_Isend

struct CN_MPI_SendBase : public PluginCNBase {
  // MPI_Isend(
  //   const void* buf,        // data set to 'send_data' -- this is done within the corresponding annotation
  //   int count,              // information on the input-arc from send_data -- where I can find it?
  //   MPI_Datatype datatype,  // data type of 'send_data' place
  //   int dest,               // it goes to the setting place
  //   int tag,                // it goes to the setting place
  //   MPI_Comm comm           // THIS IS IGNORED AND IT IS SUPPOSED TO BE MPI_COMM_WORLD
  //   MPI_Request *request    // locally stored request that is accompanied with CN's type: MessageRequest
  // );

  std::string name_prefix;
  Place &send_params;  // INPUT
  Place &send_reqst;   // OUTPUT
  Place &send_exit;    // unit exit place
  Transition &send;

  virtual ~CN_MPI_SendBase() = default;

  CN_MPI_SendBase(const CallSite &cs)
    : name_prefix("send" + get_id()),
      send_params(add_place("<empty>", "", name_prefix + "_params")),
      send_reqst(add_place("(MPI_Request, MessageRequest)", "", name_prefix + "_reqst")),
      send_exit(add_place("Unit", "", name_prefix + "_exit")),
      send(add_transition({}, name_prefix)) {

    size = cs.getArgument(1);
    datatype = cs.getArgument(2);
    dest = cs.getArgument(3);
    tag = cs.getArgument(4);

    auto non_empty_str = [] (const string &str) { return !str.empty(); };

    send_params.type = pp_vector<string>({
      compute_data_buffer_type(*datatype),
      compute_envelope_type(nullptr, dest, *tag, ",", "(", ")")}, ",", "(", ")", non_empty_str);

    add_input_edge(send_params, send, pp_vector<string>({
      compute_data_buffer_value(*datatype, *size),
      compute_envelope_value(nullptr, dest, *tag, false) }, ",", "(", ")", non_empty_str));

    add_output_edge(send, send_reqst,
                    "{" + compute_msg_rqst_value(nullptr, dest, *tag, "buffered") + "}");

    add_cf_edge(send, send_exit);
    add_cf_edge(entry_place(), send_params);
    add_cf_edge(send_exit, exit_place());
  }

  CN_MPI_SendBase(const CN_MPI_SendBase &) = delete;
  CN_MPI_SendBase(CN_MPI_SendBase &&) = default;

  virtual void connect(AddressableCN &acn) {
    add_output_edge(send, acn.asr, "{data=" + compute_data_buffer_value(*datatype, *size)
                    + ", envelope=" + compute_msg_rqst_value(nullptr, dest, *tag, "buffered") + "}");
  }

protected:
  Value const *size;
  Value const *datatype;
  Value const *dest;
  Value const *tag;
};

struct CN_MPI_Isend : public CN_MPI_SendBase {

  virtual ~CN_MPI_Isend() = default;

  CN_MPI_Isend(const CallSite &cs) : CN_MPI_SendBase(cs) {
    errs() << *cs.getInstruction() << "\n";
    mpi_rqst = cs.getArgument(6);
    assert(mpi_rqst->getType()->isPointerTy() &&
           "MPI_Request has to be treated as pointer");

    GetElementPtrInst const *gep = dyn_cast<GetElementPtrInst>(mpi_rqst);
    if (gep) {
      mpi_rqst = gep->getPointerOperand();
      add_unresolved_place(send_reqst, *mpi_rqst, create_collective_resolve_fn_());
    } else {
      add_unresolved_place(send_reqst, *mpi_rqst, create_resolve_fn_());
    }
  }

  CN_MPI_Isend(const CN_MPI_Isend &) = delete;
  CN_MPI_Isend(CN_MPI_Isend &&) = default;

private:
  UnresolvedPlace::ResolveFnTy create_resolve_fn_() {
    return [] (CommunicationNet &cn,
               Place &initiated_rqst,
               Transition &t_wait,
               UnresolvedConnect &uc) {

      cn.add_input_edge(initiated_rqst, t_wait, "(reqst, {id=id, buffered=buffered})");

      uc.close_connect(cn, &AddressableCN::get_completed_send_request,
                       "[buffered] {id=id}");
    };
  }

  UnresolvedPlace::ResolveFnTy create_collective_resolve_fn_() {
    return [] (CommunicationNet &cn, Place &, Transition &, UnresolvedConnect &uc) {

      uc.close_connect(cn, &AddressableCN::get_completed_send_request,
                       ("take(requests|(_, {id=id}),\\l"
                        "     size,\\l"
                        "     msg_tokens)\\l"));
    };
  }

  Value const *mpi_rqst;
};


// ------------------------------------------------------------------------------
// CN_MPI_Irecv

struct CN_MPI_RecvBase : public PluginCNBase { // common base class for both blocking and non-blocking calls
  // MPI_Irecv(
  //   void* buff;             // OUT; data set to 'recv_data' -- this is done via corresponding wait
  //   int count,              // IN
  //   MPI_Datatype datatype,  // IN;  data type of 'recv_data' place
  //   int source,             // IN;  it goes to the setting place
  //   int tag,                // IN;  it goes to the setting place
  //   MPI_Comm comm,          // IN;  IGNORED (for the current version it is supposed to be MPI_COMM_WORLD)
  // - MPI_Request *request    // OUT; locally stored request it is accompanied with CNs' type: MessageRequest
  // );

  std::string name_prefix;
  Place &recv_params;
  Place &recv_data;
  Place &recv_reqst;
  Place &recv_exit;
  Transition &recv;

  virtual ~CN_MPI_RecvBase() = default;

  CN_MPI_RecvBase(const CallSite &cs):
    name_prefix("recv" + get_id()),
    recv_params(add_place("<empty>", "", name_prefix + "_params")),
    recv_data(add_place("<empty>", "", name_prefix + "_data")),
    recv_reqst(add_place("(MPI_Request, MessageRequest)", "", name_prefix + "_reqst")),
    recv_exit(add_place("Unit", "", name_prefix + "_exit")),
    recv(add_transition({}, name_prefix)) {

    size = cs.getArgument(1);
    datatype = cs.getArgument(2);
    source = cs.getArgument(3);
    tag = cs.getArgument(4);

    recv_params.type = compute_envelope_type(source, nullptr, *tag, ",", "(", ")");

    recv_data.type = compute_data_buffer_type(*datatype);

    add_input_edge(recv_params, recv,
                   compute_envelope_value(source, nullptr, *tag, false, ",", "(", ")"));

    add_output_edge(recv, recv_reqst,
                    "{" + compute_msg_rqst_value(source, nullptr, *tag, "false") + "}");

    add_cf_edge(recv, recv_exit);
    add_cf_edge(entry_place(), recv_params);
    add_cf_edge(recv_exit, exit_place());
  }

  CN_MPI_RecvBase(const CN_MPI_RecvBase &) = delete;
  CN_MPI_RecvBase(CN_MPI_RecvBase &&) = default;

  virtual void connect(AddressableCN &acn) {
    add_output_edge(recv, acn.arr, compute_msg_rqst_value(source, nullptr, *tag, "false"));
  }

protected:
  Value const *size;
  Value const *datatype;
  Value const *source;
  Value const *tag;
};

struct CN_MPI_Irecv : public CN_MPI_RecvBase {

  virtual ~CN_MPI_Irecv() = default;

  CN_MPI_Irecv(const CallSite &cs) : CN_MPI_RecvBase(cs) {
    mpi_rqst = cs.getArgument(6);
    assert(mpi_rqst->getType()->isPointerTy() &&
           "MPI_Request has to be treated as pointer");

    GetElementPtrInst const *gep = dyn_cast<GetElementPtrInst>(mpi_rqst);
    if (gep) {
      mpi_rqst = gep->getPointerOperand();

      add_unresolved_place(
        recv_data, // TODO: check whether I use correct place or not?!
        *mpi_rqst,
        create_collective_resolve_fn_(recv_data, "msg_tokens|{data=data} =>* data"));
    } else {
      add_unresolved_place(
        recv_reqst, *mpi_rqst,
        create_resolve_fn_(recv_data, compute_data_buffer_value(*datatype, *size)));
    }
  }

  CN_MPI_Irecv(const CN_MPI_Irecv &) = delete;
  CN_MPI_Irecv(CN_MPI_Irecv &&) = default;

private:
  UnresolvedPlace::ResolveFnTy create_resolve_fn_(Place &recv_data, string ae_to_recv_data) {
    return [&recv_data, ae_to_recv_data] (CommunicationNet &cn,
                                          Place &initiated_rqst,
                                          Transition &t_wait,
                                          UnresolvedConnect &uc) {

      cn.add_input_edge(initiated_rqst, t_wait, "(reqst, {id=id})");
      cn.add_output_edge(t_wait, recv_data, ae_to_recv_data);

      uc.close_connect(cn, &AddressableCN::get_completed_recv_request,
                       "{data=data, envelope={id=id}}");
    };
  }

  UnresolvedPlace::ResolveFnTy create_collective_resolve_fn_(Place &recv_data, string ae_to_recv_data) {
    // NOTE: resolve for collective waits does not need to be connected as it is placed on right
    //       position because of its place within the code.
    return [&recv_data, ae_to_recv_data](CommunicationNet &cn,
                                         Place &,
                                         Transition &t_wait,
                                         UnresolvedConnect &uc) {

      cn.add_output_edge(t_wait, recv_data, ae_to_recv_data);

      uc.close_connect(cn, &AddressableCN::get_completed_recv_request,
                       ("take(requests|(_, {id=id}) =>* {envelope={id=id}},\\l"
                        "     size,\\l"
                        "     msg_tokens)\\l"));
    };
  }

  Value const *mpi_rqst;
};

// ------------------------------------------------------------------------------
// CN_MPI_Wait

struct CN_MPI_Wait final : public PluginCNBase {

  // MPI_Wait(
  //   MPI_Request *request // INOUT
  //   MPI_Status *status   // OUT
  // )

  std::string name_prefix;
  Transition &wait;
  // TODO: add status place (but only if needed)

  // NOTE: An empty constructor serves to create wait without a "real" request
  // it is resolved by knowledge of particular (blocking) call.
  CN_MPI_Wait()
    : name_prefix("wait" + get_id()),
      wait(add_transition({}, name_prefix)),
      unresolved_transition(nullptr) {

    add_cf_edge(entry_place(), wait);
    add_cf_edge(wait, exit_place());
  }

  CN_MPI_Wait(const CallSite &cs) : CN_MPI_Wait() {

    mpi_rqst = cs.getArgument(0);
    unresolved_transition = &add_unresolved_transition(wait, *mpi_rqst);
  }

  CN_MPI_Wait(const CN_MPI_Wait &) = delete;
  CN_MPI_Wait(CN_MPI_Wait &&) = default;

  void connect(AddressableCN &acn) override {
    if (unresolved_transition) {
      UnresolvedConnect uc(&acn);
      IncompleteEdge &edge = uc.incomplete_edge;
      edge.endpoint = &wait;
      edge.type = SHUFFLE;
      unresolved_transition->unresolved_connect = uc;
    }
  }

private:
  Value const *mpi_rqst;
  UnresolvedTransition *unresolved_transition;
};


// ------------------------------------------------------------------------------
// CN_MPI_Send

struct CN_MPI_Send final : public CN_MPI_SendBase {

  virtual ~CN_MPI_Send() = default;

  CN_MPI_Send(const CallSite &cs)
    : CN_MPI_SendBase(cs),
      cn_wait(),
      t_wait(cn_wait.wait) {

    add_input_edge(send_reqst, t_wait, "(reqst, {id=id})");

    add_cf_edge(exit_place(), cn_wait.entry_place());
    set_exit(cn_wait.exit_place());

    takeover(std::move(cn_wait));
  }

  CN_MPI_Send(const CN_MPI_Send &) = delete;
  CN_MPI_Send(CN_MPI_Send &&) = default;


  void connect(AddressableCN &acn) override {
    CN_MPI_SendBase::connect(acn);
    add_input_edge(acn.csr, t_wait, "[buffered] {data=data, envelope={id=id}}", SHUFFLE);
  }

private:
  CN_MPI_Wait cn_wait;
  Transition &t_wait;
};


// ------------------------------------------------------------------------------
// CN_MPI_Recv

struct CN_MPI_Recv final : public CN_MPI_RecvBase {

  virtual ~CN_MPI_Recv() = default;

  CN_MPI_Recv(const CallSite &cs)
    : CN_MPI_RecvBase(cs),
      cn_wait(),
      t_wait(cn_wait.wait) {

    Value const *size = cs.getArgument(1);
    Value const *datatype = cs.getArgument(2);

    add_input_edge(recv_reqst, t_wait, "(reqst, {id=id})");
    add_output_edge(t_wait, recv_data,
                    compute_data_buffer_value(*datatype, *size));

    add_cf_edge(exit_place(), cn_wait.entry_place());
    set_exit(cn_wait.exit_place());

    takeover(std::move(cn_wait));
  }

  CN_MPI_Recv(const CN_MPI_Recv &) = delete;
  CN_MPI_Recv(CN_MPI_Recv &&) = default;

  void connect (AddressableCN &acn) {
    CN_MPI_RecvBase::connect(acn);
    add_input_edge(acn.crr, t_wait, "{data=data, envelope={id=id}}", SHUFFLE);
  }

private:
  CN_MPI_Wait cn_wait;
  Transition &t_wait;
};


// -----------------------------------------------------------------------------
// CN_MPI_Waitall

struct CN_MPI_Waitall final : public PluginCNBase {

  // MPI_Waitall(
  //   int count                        // IN
  //   MPI_Request array_of_requests[]  // INOUT
  //   MPI_Status array_of_statuses[]   // OUT
  // )

  std::string name_prefix;
  Place &waitall_count;
  Place &waitall_rqsts;
  Transition &waitall;
  // TODO: place with statuses

  CN_MPI_Waitall(const CallSite &cs)
    : name_prefix("waitall" + get_id()),
      waitall_count(add_place("Int", "", name_prefix + "_count")),
      waitall_rqsts(add_place("(MPI_Request, MessageRequest)", "", name_prefix + "_reqsts")),
      waitall(add_transition({}, name_prefix)) {

    // connect entry and exit points
    add_cf_edge(entry_place(), waitall);
    add_cf_edge(waitall, exit_place());
    add_input_edge(waitall_count, waitall, "size");
    add_input_edge(waitall_rqsts, waitall, "take(_, size, requests)");

    mpi_rqsts = cs.getArgument(1);
    unresolved_transition = &add_unresolved_transition(waitall, *mpi_rqsts);
  }

  CN_MPI_Waitall(const CN_MPI_Waitall &) = delete;
  CN_MPI_Waitall(CN_MPI_Waitall &&) = default;

  void connect(AddressableCN &acn) override {
    if (unresolved_transition) {
      UnresolvedConnect uc(&acn);
      IncompleteEdge &edge = uc.incomplete_edge;
      edge.endpoint = &waitall;
      edge.type = SHUFFLE;
      unresolved_transition->unresolved_connect = uc;
    }
  }

private:
  Value const *mpi_rqsts;
  UnresolvedTransition *unresolved_transition;
};

// ===========================================================================
// CNs factory

PluginCNGeneric createCommSubnet(const CallSite &cs) {
  Function *f = cs.getCalledFunction();
  assert (f->hasName() && "The CNFactory expects call site with a named function");

  StringRef call_name = f->getName();
  if (call_name == "MPI_Isend") {
    return CN_MPI_Isend(cs);
  } else if (call_name == "MPI_Send") {
    return CN_MPI_Send(cs);
  } else if (call_name == "MPI_Irecv") {
    return CN_MPI_Irecv(cs);
  } else if (call_name == "MPI_Recv") {
    return CN_MPI_Recv(cs);
  } else if (call_name == "MPI_Wait") {
    return CN_MPI_Wait(cs);
  } else if (call_name == "MPI_Waitall") {
    return CN_MPI_Waitall(cs);
  }
  return EmptyCN(cs);
}

} // end of anonymous namespace
#endif // COMM_NET_FACTORY_H
