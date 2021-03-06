/*********************                                                        */
/*! \file proof_post_processor.cpp
 ** \verbatim
 ** Top contributors (to current version):
 **   Andrew Reynolds
 ** This file is part of the CVC4 project.
 ** Copyright (c) 2009-2020 by the authors listed in the file AUTHORS
 ** in the top-level source directory and their institutional affiliations.
 ** All rights reserved.  See the file COPYING in the top-level source
 ** directory for licensing information.\endverbatim
 **
 ** \brief Implementation of module for processing proof nodes
 **/

#include "smt/proof_post_processor.h"

#include "expr/skolem_manager.h"
#include "options/smt_options.h"
#include "preprocessing/assertion_pipeline.h"
#include "smt/smt_engine.h"
#include "smt/smt_statistics_registry.h"
#include "theory/builtin/proof_checker.h"
#include "theory/rewriter.h"
#include "theory/theory.h"

using namespace CVC4::kind;
using namespace CVC4::theory;

namespace CVC4 {
namespace smt {

ProofPostprocessCallback::ProofPostprocessCallback(ProofNodeManager* pnm,
                                                   SmtEngine* smte,
                                                   ProofGenerator* pppg)
    : d_pnm(pnm), d_smte(smte), d_pppg(pppg), d_wfpm(pnm)
{
  d_true = NodeManager::currentNM()->mkConst(true);
  // always check whether to update ASSUME
  d_elimRules.insert(PfRule::ASSUME);
}

void ProofPostprocessCallback::initializeUpdate()
{
  d_assumpToProof.clear();
  d_wfAssumptions.clear();
}

void ProofPostprocessCallback::setEliminateRule(PfRule rule)
{
  d_elimRules.insert(rule);
}

bool ProofPostprocessCallback::shouldUpdate(std::shared_ptr<ProofNode> pn,
                                            bool& continueUpdate)
{
  return d_elimRules.find(pn->getRule()) != d_elimRules.end();
}

bool ProofPostprocessCallback::update(Node res,
                                      PfRule id,
                                      const std::vector<Node>& children,
                                      const std::vector<Node>& args,
                                      CDProof* cdp,
                                      bool& continueUpdate)
{
  Trace("smt-proof-pp-debug") << "- Post process " << id << " " << children
                              << " / " << args << std::endl;

  if (id == PfRule::ASSUME)
  {
    // we cache based on the assumption node, not the proof node, since there
    // may be multiple occurrences of the same node.
    Node f = args[0];
    std::shared_ptr<ProofNode> pfn;
    std::map<Node, std::shared_ptr<ProofNode>>::iterator it =
        d_assumpToProof.find(f);
    if (it != d_assumpToProof.end())
    {
      Trace("smt-proof-pp-debug") << "...already computed" << std::endl;
      pfn = it->second;
    }
    else
    {
      Trace("smt-proof-pp-debug") << "...get proof" << std::endl;
      Assert(d_pppg != nullptr);
      // get proof from preprocess proof generator
      pfn = d_pppg->getProofFor(f);
      Trace("smt-proof-pp-debug") << "...finished get proof" << std::endl;
      // print for debugging
      if (pfn == nullptr)
      {
        Trace("smt-proof-pp-debug")
            << "...no proof, possibly an input assumption" << std::endl;
      }
      else
      {
        Assert(pfn->getResult() == f);
        if (Trace.isOn("smt-proof-pp"))
        {
          Trace("smt-proof-pp")
              << "=== Connect proof for preprocessing: " << f << std::endl;
          Trace("smt-proof-pp") << *pfn.get() << std::endl;
        }
      }
      d_assumpToProof[f] = pfn;
    }
    if (pfn == nullptr || pfn->getRule() == PfRule::ASSUME)
    {
      Trace("smt-proof-pp-debug") << "...do not add proof" << std::endl;
      // no update
      return false;
    }
    Trace("smt-proof-pp-debug") << "...add proof" << std::endl;
    // connect the proof
    cdp->addProof(pfn);
    return true;
  }
  Node ret = expandMacros(id, children, args, cdp);
  Trace("smt-proof-pp-debug") << "...expanded = " << !ret.isNull() << std::endl;
  return !ret.isNull();
}

bool ProofPostprocessCallback::updateInternal(Node res,
                                              PfRule id,
                                              const std::vector<Node>& children,
                                              const std::vector<Node>& args,
                                              CDProof* cdp)
{
  bool continueUpdate = true;
  return update(res, id, children, args, cdp, continueUpdate);
}

Node ProofPostprocessCallback::expandMacros(PfRule id,
                                            const std::vector<Node>& children,
                                            const std::vector<Node>& args,
                                            CDProof* cdp)
{
  if (d_elimRules.find(id) == d_elimRules.end())
  {
    // not eliminated
    return Node::null();
  }
  // macro elimination
  if (id == PfRule::MACRO_SR_EQ_INTRO)
  {
    // (TRANS
    //   (SUBS <children> :args args[0:1])
    //   (REWRITE :args <t.substitute(x1,t1). ... .substitute(xn,tn)> args[2]))
    std::vector<Node> tchildren;
    Node t = args[0];
    Node ts;
    if (!children.empty())
    {
      std::vector<Node> sargs;
      sargs.push_back(t);
      MethodId sid = MethodId::SB_DEFAULT;
      if (args.size() >= 2)
      {
        if (builtin::BuiltinProofRuleChecker::getMethodId(args[1], sid))
        {
          sargs.push_back(args[1]);
        }
      }
      ts =
          builtin::BuiltinProofRuleChecker::applySubstitution(t, children, sid);
      Trace("smt-proof-pp-debug")
          << "...eq intro subs equality is " << t << " == " << ts << ", from "
          << sid << std::endl;
      if (ts != t)
      {
        Node eq = t.eqNode(ts);
        // apply SUBS proof rule if necessary
        if (!updateInternal(eq, PfRule::SUBS, children, sargs, cdp))
        {
          // if we specified that we did not want to eliminate, add as step
          cdp->addStep(eq, PfRule::SUBS, children, sargs);
        }
        tchildren.push_back(eq);
      }
    }
    else
    {
      // no substitute
      ts = t;
    }
    std::vector<Node> rargs;
    rargs.push_back(ts);
    MethodId rid = MethodId::RW_REWRITE;
    if (args.size() >= 3)
    {
      if (builtin::BuiltinProofRuleChecker::getMethodId(args[2], rid))
      {
        rargs.push_back(args[2]);
      }
    }
    builtin::BuiltinProofRuleChecker* builtinPfC =
        static_cast<builtin::BuiltinProofRuleChecker*>(
            d_pnm->getChecker()->getCheckerFor(PfRule::MACRO_SR_EQ_INTRO));
    Node tr = builtinPfC->applyRewrite(ts, rid);
    Trace("smt-proof-pp-debug")
        << "...eq intro rewrite equality is " << ts << " == " << tr << ", from "
        << rid << std::endl;
    if (ts != tr)
    {
      Node eq = ts.eqNode(tr);
      // apply REWRITE proof rule
      if (!updateInternal(eq, PfRule::REWRITE, {}, rargs, cdp))
      {
        // if not elimianted, add as step
        cdp->addStep(eq, PfRule::REWRITE, {}, rargs);
      }
      tchildren.push_back(eq);
    }
    if (t == tr)
    {
      // typically not necessary, but done to be robust
      cdp->addStep(t.eqNode(tr), PfRule::REFL, {}, {t});
      return t.eqNode(tr);
    }
    // must add TRANS if two step
    return addProofForTrans(tchildren, cdp);
  }
  else if (id == PfRule::MACRO_SR_PRED_INTRO)
  {
    std::vector<Node> tchildren;
    std::vector<Node> sargs = args;
    // take into account witness form, if necessary
    bool reqWitness = d_wfpm.requiresWitnessFormIntro(args[0]);
    Trace("smt-proof-pp-debug")
        << "...pred intro reqWitness=" << reqWitness << std::endl;
    // (TRUE_ELIM
    // (TRANS
    //    (MACRO_SR_EQ_INTRO <children> :args (t args[1:]))
    //    ... proof of apply_SR(t) = toWitness(apply_SR(t)) ...
    //    (MACRO_SR_EQ_INTRO {} {toWitness(apply_SR(t))})
    // ))
    // Notice this is an optimized, one sided version of the expansion of
    // MACRO_SR_PRED_TRANSFORM below.
    // We call the expandMacros method on MACRO_SR_EQ_INTRO, where notice
    // that this rule application is immediately expanded in the recursive
    // call and not added to the proof.
    Node conc = expandMacros(PfRule::MACRO_SR_EQ_INTRO, children, sargs, cdp);
    Trace("smt-proof-pp-debug")
        << "...pred intro conclusion is " << conc << std::endl;
    Assert(!conc.isNull());
    Assert(conc.getKind() == EQUAL);
    Assert(conc[0] == args[0]);
    tchildren.push_back(conc);
    if (reqWitness)
    {
      Node weq = addProofForWitnessForm(conc[1], cdp);
      Trace("smt-proof-pp-debug") << "...weq is " << weq << std::endl;
      if (addToTransChildren(weq, tchildren))
      {
        // toWitness(apply_SR(t)) = apply_SR(toWitness(apply_SR(t)))
        // rewrite again, don't need substitution. Also we always use the
        // default rewriter, due to the definition of MACRO_SR_PRED_INTRO.
        Node weqr = expandMacros(PfRule::MACRO_SR_EQ_INTRO, {}, {weq[1]}, cdp);
        addToTransChildren(weqr, tchildren);
      }
    }
    // apply transitivity if necessary
    Node eq = addProofForTrans(tchildren, cdp);
    Assert(!eq.isNull());
    Assert(eq.getKind() == EQUAL);
    Assert(eq[0] == args[0]);
    Assert(eq[1] == d_true);

    cdp->addStep(eq[0], PfRule::TRUE_ELIM, {eq}, {});
    return eq[0];
  }
  else if (id == PfRule::MACRO_SR_PRED_ELIM)
  {
    // (EQ_RESOLVE
    //   children[0]
    //   (MACRO_SR_EQ_INTRO children[1:] :args children[0] ++ args))
    std::vector<Node> schildren(children.begin() + 1, children.end());
    std::vector<Node> srargs;
    srargs.push_back(children[0]);
    srargs.insert(srargs.end(), args.begin(), args.end());
    Node conc = expandMacros(PfRule::MACRO_SR_EQ_INTRO, schildren, srargs, cdp);
    Assert(!conc.isNull());
    Assert(conc.getKind() == EQUAL);
    Assert(conc[0] == children[0]);
    // apply equality resolve
    cdp->addStep(conc[1], PfRule::EQ_RESOLVE, {children[0], conc}, {});
    return conc[1];
  }
  else if (id == PfRule::MACRO_SR_PRED_TRANSFORM)
  {
    // (EQ_RESOLVE
    //   children[0]
    //   (TRANS
    //      (MACRO_SR_EQ_INTRO children[1:] :args (children[0] args[1:]))
    //      ... proof of c = wc
    //      (MACRO_SR_EQ_INTRO {} wc)
    //      (SYMM
    //        (MACRO_SR_EQ_INTRO children[1:] :args <args>)
    //        ... proof of a = wa
    //        (MACRO_SR_EQ_INTRO {} wa))))
    // where
    // wa = toWitness(apply_SR(args[0])) and
    // wc = toWitness(apply_SR(children[0])).
    Trace("smt-proof-pp-debug")
        << "Transform " << children[0] << " == " << args[0] << std::endl;
    if (CDProof::isSame(children[0], args[0]))
    {
      Trace("smt-proof-pp-debug") << "...nothing to do" << std::endl;
      // nothing to do
      return children[0];
    }
    std::vector<Node> tchildren;
    std::vector<Node> schildren(children.begin() + 1, children.end());
    std::vector<Node> sargs = args;
    // first, compute if we need
    bool reqWitness = d_wfpm.requiresWitnessFormTransform(children[0], args[0]);
    Trace("smt-proof-pp-debug") << "...reqWitness=" << reqWitness << std::endl;
    // convert both sides, in three steps, take symmetry of second chain
    for (unsigned r = 0; r < 2; r++)
    {
      std::vector<Node> tchildrenr;
      // first rewrite children[0], then args[0]
      sargs[0] = r == 0 ? children[0] : args[0];
      // t = apply_SR(t)
      Node eq = expandMacros(PfRule::MACRO_SR_EQ_INTRO, schildren, sargs, cdp);
      Trace("smt-proof-pp-debug")
          << "transform subs_rewrite (" << r << "): " << eq << std::endl;
      Assert(!eq.isNull() && eq.getKind() == EQUAL && eq[0] == sargs[0]);
      addToTransChildren(eq, tchildrenr);
      // apply_SR(t) = toWitness(apply_SR(t))
      if (reqWitness)
      {
        Node weq = addProofForWitnessForm(eq[1], cdp);
        Trace("smt-proof-pp-debug")
            << "transform toWitness (" << r << "): " << weq << std::endl;
        if (addToTransChildren(weq, tchildrenr))
        {
          // toWitness(apply_SR(t)) = apply_SR(toWitness(apply_SR(t)))
          // rewrite again, don't need substitution. Also, we always use the
          // default rewriter, due to the definition of MACRO_SR_PRED_TRANSFORM.
          Node weqr =
              expandMacros(PfRule::MACRO_SR_EQ_INTRO, {}, {weq[1]}, cdp);
          Trace("smt-proof-pp-debug") << "transform rewrite_witness (" << r
                                      << "): " << weqr << std::endl;
          addToTransChildren(weqr, tchildrenr);
        }
      }
      Trace("smt-proof-pp-debug")
          << "transform connect (" << r << ")" << std::endl;
      // add to overall chain
      if (r == 0)
      {
        // add the current chain to the overall chain
        tchildren.insert(tchildren.end(), tchildrenr.begin(), tchildrenr.end());
      }
      else
      {
        // add the current chain to cdp
        Node eqr = addProofForTrans(tchildrenr, cdp);
        if (!eqr.isNull())
        {
          Trace("smt-proof-pp-debug") << "transform connect sym " << tchildren
                                      << " " << eqr << std::endl;
          // take symmetry of above and add it to the overall chain
          addToTransChildren(eqr, tchildren, true);
        }
      }
      Trace("smt-proof-pp-debug")
          << "transform finish (" << r << ")" << std::endl;
    }

    // apply transitivity if necessary
    Node eq = addProofForTrans(tchildren, cdp);

    cdp->addStep(args[0], PfRule::EQ_RESOLVE, {children[0], eq}, {});
    return args[0];
  }
  else if (id == PfRule::SUBS)
  {
    NodeManager* nm = NodeManager::currentNM();
    // Notice that a naive way to reconstruct SUBS is to do a term conversion
    // proof for each substitution.
    // The proof of f(a) * { a -> g(b) } * { b -> c } = f(g(c)) is:
    //   TRANS( CONG{f}( a=g(b) ), CONG{f}( CONG{g}( b=c ) ) )
    // Notice that more optimal proofs are possible that do a single traversal
    // over t. This is done by applying later substitutions to the range of
    // previous substitutions, until a final simultaneous substitution is
    // applied to t.  For instance, in the above example, we first prove:
    //   CONG{g}( b = c )
    // by applying the second substitution { b -> c } to the range of the first,
    // giving us a proof of g(b)=g(c). We then construct the updated proof
    // by tranitivity:
    //   TRANS( a=g(b), CONG{g}( b=c ) )
    // We then apply the substitution { a -> g(c), b -> c } to f(a), to obtain:
    //   CONG{f}( TRANS( a=g(b), CONG{g}( b=c ) ) )
    // which notice is more compact than the proof above.
    Node t = args[0];
    // get the kind of substitution
    MethodId ids = MethodId::SB_DEFAULT;
    if (args.size() >= 2)
    {
      builtin::BuiltinProofRuleChecker::getMethodId(args[1], ids);
    }
    std::vector<std::shared_ptr<CDProof>> pfs;
    std::vector<TNode> vsList;
    std::vector<TNode> ssList;
    std::vector<TNode> fromList;
    std::vector<ProofGenerator*> pgs;
    // first, compute the entire substitution
    for (size_t i = 0, nchild = children.size(); i < nchild; i++)
    {
      // get the substitution
      builtin::BuiltinProofRuleChecker::getSubstitutionFor(
          children[i], vsList, ssList, fromList, ids);
      // ensure proofs for each formula in fromList
      if (children[i].getKind() == AND && ids == MethodId::SB_DEFAULT)
      {
        for (size_t j = 0, nchildi = children[i].getNumChildren(); j < nchildi;
             j++)
        {
          Node nodej = nm->mkConst(Rational(j));
          cdp->addStep(
              children[i][j], PfRule::AND_ELIM, {children[i]}, {nodej});
        }
      }
    }
    std::vector<Node> vvec;
    std::vector<Node> svec;
    for (size_t i = 0, nvs = vsList.size(); i < nvs; i++)
    {
      // Note we process in forward order, since later substitution should be
      // applied to earlier ones, and the last child of a SUBS is processed
      // first.
      TNode var = vsList[i];
      TNode subs = ssList[i];
      TNode childFrom = fromList[i];
      Trace("smt-proof-pp-debug")
          << "...process " << var << " -> " << subs << " (" << childFrom << ", "
          << ids << ")" << std::endl;
      // apply the current substitution to the range
      if (!vvec.empty())
      {
        Node ss =
            subs.substitute(vvec.begin(), vvec.end(), svec.begin(), svec.end());
        if (ss != subs)
        {
          Trace("smt-proof-pp-debug")
              << "......updated to " << var << " -> " << ss
              << " based on previous substitution" << std::endl;
          // make the proof for the tranitivity step
          std::shared_ptr<CDProof> pf = std::make_shared<CDProof>(d_pnm);
          pfs.push_back(pf);
          // prove the updated substitution
          TConvProofGenerator tcg(d_pnm,
                                  nullptr,
                                  TConvPolicy::ONCE,
                                  TConvCachePolicy::NEVER,
                                  "nested_SUBS_TConvProofGenerator",
                                  nullptr,
                                  true);
          // add previous rewrite steps
          for (unsigned j = 0, nvars = vvec.size(); j < nvars; j++)
          {
            tcg.addRewriteStep(vvec[j], svec[j], pgs[j]);
          }
          // get the proof for the update to the current substitution
          Node seqss = subs.eqNode(ss);
          std::shared_ptr<ProofNode> pfn = tcg.getProofFor(seqss);
          Assert(pfn != nullptr);
          // add the proof
          pf->addProof(pfn);
          // get proof for childFrom from cdp
          pfn = cdp->getProofFor(childFrom);
          pf->addProof(pfn);
          // ensure we have a proof of var = subs
          Node veqs = addProofForSubsStep(var, subs, childFrom, pf.get());
          // transitivity
          pf->addStep(var.eqNode(ss), PfRule::TRANS, {veqs, seqss}, {});
          // add to the substitution
          vvec.push_back(var);
          svec.push_back(ss);
          pgs.push_back(pf.get());
          continue;
        }
      }
      // Just use equality from CDProof, but ensure we have a proof in cdp.
      // This may involve a TRUE_INTRO/FALSE_INTRO if the substitution step
      // uses the assumption childFrom as a Boolean assignment (e.g.
      // childFrom = true if we are using MethodId::SB_LITERAL).
      addProofForSubsStep(var, subs, childFrom, cdp);
      vvec.push_back(var);
      svec.push_back(subs);
      pgs.push_back(cdp);
    }
    Node ts = t.substitute(vvec.begin(), vvec.end(), svec.begin(), svec.end());
    Node eq = t.eqNode(ts);
    if (ts != t)
    {
      // should be implied by the substitution now
      TConvProofGenerator tcpg(d_pnm,
                               nullptr,
                               TConvPolicy::ONCE,
                               TConvCachePolicy::NEVER,
                               "SUBS_TConvProofGenerator",
                               nullptr,
                               true);
      for (unsigned j = 0, nvars = vvec.size(); j < nvars; j++)
      {
        tcpg.addRewriteStep(vvec[j], svec[j], pgs[j]);
      }
      // add the proof constructed by the term conversion utility
      std::shared_ptr<ProofNode> pfn = tcpg.getProofFor(eq);
      // should give a proof, if not, then tcpg does not agree with the
      // substitution.
      Assert(pfn != nullptr);
      if (pfn == nullptr)
      {
        cdp->addStep(eq, PfRule::TRUST_SUBS, {}, {eq});
      }
      else
      {
        cdp->addProof(pfn);
      }
    }
    else
    {
      // should not be necessary typically
      cdp->addStep(eq, PfRule::REFL, {}, {t});
    }
    return eq;
  }
  else if (id == PfRule::REWRITE)
  {
    // get the kind of rewrite
    MethodId idr = MethodId::RW_REWRITE;
    if (args.size() >= 2)
    {
      builtin::BuiltinProofRuleChecker::getMethodId(args[1], idr);
    }
    builtin::BuiltinProofRuleChecker* builtinPfC =
        static_cast<builtin::BuiltinProofRuleChecker*>(
            d_pnm->getChecker()->getCheckerFor(PfRule::REWRITE));
    Node ret = builtinPfC->applyRewrite(args[0], idr);
    Node eq = args[0].eqNode(ret);
    if (idr == MethodId::RW_REWRITE || idr == MethodId::RW_REWRITE_EQ_EXT)
    {
      // rewrites from theory::Rewriter
      bool isExtEq = (idr == MethodId::RW_REWRITE_EQ_EXT);
      // use rewrite with proof interface
      Rewriter* rr = d_smte->getRewriter();
      TrustNode trn = rr->rewriteWithProof(args[0], isExtEq);
      std::shared_ptr<ProofNode> pfn = trn.toProofNode();
      if (pfn == nullptr)
      {
        Trace("smt-proof-pp-debug")
            << "Use TRUST_REWRITE for " << eq << std::endl;
        // did not have a proof of rewriting, probably isExtEq is true
        if (isExtEq)
        {
          // update to THEORY_REWRITE with idr
          Assert(args.size() >= 1);
          TheoryId theoryId = Theory::theoryOf(args[0].getType());
          Node tid = builtin::BuiltinProofRuleChecker::mkTheoryIdNode(theoryId);
          cdp->addStep(eq, PfRule::THEORY_REWRITE, {}, {eq, tid, args[1]});
        }
        else
        {
          // this should never be applied
          cdp->addStep(eq, PfRule::TRUST_REWRITE, {}, {eq});
        }
      }
      else
      {
        cdp->addProof(pfn);
      }
      Assert(trn.getNode() == ret)
          << "Unexpected rewrite " << args[0] << std::endl
          << "Got: " << trn.getNode() << std::endl
          << "Expected: " << ret;
    }
    else if (idr == MethodId::RW_EVALUATE)
    {
      // change to evaluate, which is never eliminated
      cdp->addStep(eq, PfRule::EVALUATE, {}, {args[0]});
    }
    else
    {
      // don't know how to eliminate
      return Node::null();
    }
    if (args[0] == ret)
    {
      // should not be necessary typically
      cdp->addStep(eq, PfRule::REFL, {}, {args[0]});
    }
    return eq;
  }
  else if (id == PfRule::THEORY_REWRITE)
  {
    Assert(!args.empty());
    Node eq = args[0];
    Assert(eq.getKind() == EQUAL);
    // try to replay theory rewrite
    // first, check that maybe its just an evaluation step
    ProofChecker* pc = d_pnm->getChecker();
    Node ceval =
        pc->checkDebug(PfRule::EVALUATE, {}, {eq[0]}, eq, "smt-proof-pp-debug");
    if (!ceval.isNull() && ceval == eq)
    {
      cdp->addStep(eq, PfRule::EVALUATE, {}, {eq[0]});
      return eq;
    }
    // otherwise no update
    Trace("final-pf-hole") << "hole: " << id << " : " << eq << std::endl;
  }

  // TRUST, PREPROCESS, THEORY_LEMMA, THEORY_PREPROCESS?

  return Node::null();
}

Node ProofPostprocessCallback::addProofForWitnessForm(Node t, CDProof* cdp)
{
  Node tw = SkolemManager::getWitnessForm(t);
  Node eq = t.eqNode(tw);
  if (t == tw)
  {
    // not necessary, add REFL step
    cdp->addStep(eq, PfRule::REFL, {}, {t});
    return eq;
  }
  std::shared_ptr<ProofNode> pn = d_wfpm.getProofFor(eq);
  if (pn != nullptr)
  {
    // add the proof
    cdp->addProof(pn);
  }
  else
  {
    Assert(false) << "ProofPostprocessCallback::addProofForWitnessForm: failed "
                     "to add proof for witness form of "
                  << t;
  }
  return eq;
}

Node ProofPostprocessCallback::addProofForTrans(
    const std::vector<Node>& tchildren, CDProof* cdp)
{
  size_t tsize = tchildren.size();
  if (tsize > 1)
  {
    Node lhs = tchildren[0][0];
    Node rhs = tchildren[tsize - 1][1];
    Node eq = lhs.eqNode(rhs);
    cdp->addStep(eq, PfRule::TRANS, tchildren, {});
    return eq;
  }
  else if (tsize == 1)
  {
    return tchildren[0];
  }
  return Node::null();
}

Node ProofPostprocessCallback::addProofForSubsStep(Node var,
                                                   Node subs,
                                                   Node assump,
                                                   CDProof* cdp)
{
  // ensure we have a proof of var = subs
  Node veqs = var.eqNode(subs);
  if (veqs != assump)
  {
    // should be true intro or false intro
    Assert(subs.isConst());
    cdp->addStep(
        veqs,
        subs.getConst<bool>() ? PfRule::TRUE_INTRO : PfRule::FALSE_INTRO,
        {assump},
        {});
  }
  return veqs;
}

bool ProofPostprocessCallback::addToTransChildren(Node eq,
                                                  std::vector<Node>& tchildren,
                                                  bool isSymm)
{
  Assert(!eq.isNull());
  Assert(eq.getKind() == kind::EQUAL);
  if (eq[0] == eq[1])
  {
    return false;
  }
  Node equ = isSymm ? eq[1].eqNode(eq[0]) : eq;
  Assert(tchildren.empty()
         || (tchildren[tchildren.size() - 1].getKind() == kind::EQUAL
             && tchildren[tchildren.size() - 1][1] == equ[0]));
  tchildren.push_back(equ);
  return true;
}

ProofPostprocessFinalCallback::ProofPostprocessFinalCallback(
    ProofNodeManager* pnm)
    : d_ruleCount("finalProof::ruleCount"),
      d_totalRuleCount("finalProof::totalRuleCount", 0),
      d_minPedanticLevel("finalProof::minPedanticLevel", 10),
      d_numFinalProofs("finalProofs::numFinalProofs", 0),
      d_pnm(pnm),
      d_pedanticFailure(false)
{
  smtStatisticsRegistry()->registerStat(&d_ruleCount);
  smtStatisticsRegistry()->registerStat(&d_totalRuleCount);
  smtStatisticsRegistry()->registerStat(&d_minPedanticLevel);
  smtStatisticsRegistry()->registerStat(&d_numFinalProofs);
}

ProofPostprocessFinalCallback::~ProofPostprocessFinalCallback()
{
  smtStatisticsRegistry()->unregisterStat(&d_ruleCount);
  smtStatisticsRegistry()->unregisterStat(&d_totalRuleCount);
  smtStatisticsRegistry()->unregisterStat(&d_minPedanticLevel);
  smtStatisticsRegistry()->unregisterStat(&d_numFinalProofs);
}

void ProofPostprocessFinalCallback::initializeUpdate()
{
  d_pedanticFailure = false;
  d_pedanticFailureOut.str("");
  ++d_numFinalProofs;
}

bool ProofPostprocessFinalCallback::shouldUpdate(std::shared_ptr<ProofNode> pn,
                                                 bool& continueUpdate)
{
  PfRule r = pn->getRule();
  // if not doing eager pedantic checking, fail if below threshold
  if (!options::proofNewEagerChecking())
  {
    if (!d_pedanticFailure)
    {
      Assert(d_pedanticFailureOut.str().empty());
      if (d_pnm->getChecker()->isPedanticFailure(r, d_pedanticFailureOut))
      {
        d_pedanticFailure = true;
      }
    }
  }
  uint32_t plevel = d_pnm->getChecker()->getPedanticLevel(r);
  if (plevel != 0)
  {
    d_minPedanticLevel.minAssign(plevel);
  }
  // record stats for the rule
  d_ruleCount << r;
  ++d_totalRuleCount;
  return false;
}

bool ProofPostprocessFinalCallback::wasPedanticFailure(std::ostream& out) const
{
  if (d_pedanticFailure)
  {
    out << d_pedanticFailureOut.str();
    return true;
  }
  return false;
}

ProofPostproccess::ProofPostproccess(ProofNodeManager* pnm,
                                     SmtEngine* smte,
                                     ProofGenerator* pppg)
    : d_pnm(pnm),
      d_cb(pnm, smte, pppg),
      // the update merges subproofs
      d_updater(d_pnm, d_cb, true),
      d_finalCb(pnm),
      d_finalizer(d_pnm, d_finalCb)
{
}

ProofPostproccess::~ProofPostproccess() {}

void ProofPostproccess::process(std::shared_ptr<ProofNode> pf)
{
  // Initialize the callback, which computes necessary static information about
  // how to process, including how to process assumptions in pf.
  d_cb.initializeUpdate();
  // now, process
  d_updater.process(pf);
  // take stats and check pedantic
  d_finalCb.initializeUpdate();
  d_finalizer.process(pf);

  std::stringstream serr;
  bool wasPedanticFailure = d_finalCb.wasPedanticFailure(serr);
  if (wasPedanticFailure)
  {
    AlwaysAssert(!wasPedanticFailure)
        << "ProofPostproccess::process: pedantic failure:" << std::endl
        << serr.str();
  }
}

void ProofPostproccess::setEliminateRule(PfRule rule)
{
  d_cb.setEliminateRule(rule);
}

}  // namespace smt
}  // namespace CVC4
