#include "grpc_request_proxy.h" 
 
#include "rpc_calls.h" 
#include "rpc_kqp_base.h" 
#include "rpc_common.h"
 
#include <ydb/library/yql/public/issue/yql_issue_message.h>
#include <ydb/library/yql/public/issue/yql_issue.h>
 
namespace NKikimr { 
namespace NGRpcService { 
 
using namespace NActors; 
using namespace Ydb; 
using namespace NKqp; 
 
class TRollbackTransactionRPC : public TRpcKqpRequestActor<TRollbackTransactionRPC, TEvRollbackTransactionRequest> { 
    using TBase = TRpcKqpRequestActor<TRollbackTransactionRPC, TEvRollbackTransactionRequest>; 
 
public: 
    TRollbackTransactionRPC(TEvRollbackTransactionRequest* msg) 
        : TBase(msg) {} 
 
    void Bootstrap(const TActorContext& ctx) { 
        TBase::Bootstrap(ctx); 
 
        RollbackTransactionImpl(ctx); 
        Become(&TRollbackTransactionRPC::StateWork); 
    } 
 
    void StateWork(TAutoPtr<IEventHandle>& ev, const TActorContext& ctx) { 
        switch (ev->GetTypeRewrite()) { 
            HFunc(NKqp::TEvKqp::TEvQueryResponse, Handle); 
            default: TBase::StateWork(ev, ctx); 
        } 
    } 
 
private: 
    void RollbackTransactionImpl(const TActorContext &ctx) { 
        const auto req = GetProtoRequest();
        const auto traceId = Request_->GetTraceId(); 
 
        TString sessionId; 
        auto ev = MakeHolder<NKqp::TEvKqp::TEvQueryRequest>(); 
        SetAuthToken(ev, *Request_);
        SetDatabase(ev, *Request_);

        NYql::TIssues issues; 
        if (CheckSession(req->session_id(), issues)) { 
            ev->Record.MutableRequest()->SetSessionId(req->session_id()); 
        } else { 
            return Reply(Ydb::StatusIds::BAD_REQUEST, issues, ctx); 
        } 
 
        if (traceId) { 
            ev->Record.SetTraceId(traceId.GetRef()); 
        } 
 
        if (!req->tx_id()) { 
            NYql::TIssues issues; 
            issues.AddIssue(MakeIssue(NKikimrIssues::TIssuesIds::DEFAULT_ERROR, "Empty transaction id.")); 
            return Reply(Ydb::StatusIds::BAD_REQUEST, issues, ctx); 
        } 
 
        ev->Record.MutableRequest()->SetAction(NKikimrKqp::QUERY_ACTION_ROLLBACK_TX); 
        ev->Record.MutableRequest()->MutableTxControl()->set_tx_id(req->tx_id()); 
 
        ctx.Send(NKqp::MakeKqpProxyID(ctx.SelfID.NodeId()), ev.Release()); 
    } 
 
    void Handle(NKqp::TEvKqp::TEvQueryResponse::TPtr& ev, const TActorContext& ctx) { 
        const auto& record = ev->Get()->Record.GetRef(); 
        AddServerHintsIfAny(record);

        if (record.GetYdbStatus() == Ydb::StatusIds::SUCCESS) { 
            const auto& kqpResponse = record.GetResponse(); 
            const auto& issueMessage = kqpResponse.GetQueryIssues(); 
 
            ReplyWithResult(Ydb::StatusIds::SUCCESS, issueMessage, ctx); 
        } else { 
            return OnGenericQueryResponseError(record, ctx); 
        } 
    } 
 
    void ReplyWithResult(StatusIds::StatusCode status, 
                         const google::protobuf::RepeatedPtrField<TYdbIssueMessageType>& message,
                         const TActorContext& ctx) { 
        Request_->SendResult(status, message); 
        Die(ctx); 
    } 
}; 
 
void TGRpcRequestProxy::Handle(TEvRollbackTransactionRequest::TPtr& ev, const TActorContext& ctx) { 
    ctx.Register(new TRollbackTransactionRPC(ev->Release().Release())); 
} 
 
} // namespace NGRpcService 
} // namespace NKikimr 
