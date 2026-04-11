//===--- PotentialNullPtrDerefCheck.cpp - clang-tidy ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PotentialNullPtrDerefCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Analysis/FlowSensitive/CFGMatchSwitch.h"
#include "clang/Analysis/FlowSensitive/DataflowAnalysis.h"
#include "clang/Analysis/FlowSensitive/DataflowEnvironment.h"
#include "clang/Analysis/FlowSensitive/Formula.h"
#include "clang/Analysis/FlowSensitive/NoopLattice.h"
#include "clang/Analysis/FlowSensitive/StorageLocation.h"
#include "clang/Analysis/FlowSensitive/Value.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Error.h"

namespace clang::tidy::haha {

using namespace ast_matchers;
using dataflow::TransferStateForDiagnostics;

namespace {

constexpr llvm::StringLiteral FuncID = "func";

// Nullness property name stored on pointer values
constexpr llvm::StringLiteral IsNullProp = "is_null";

// Get or create the nullness property for a pointer value
BoolValue *getOrCreateNullness(PointerValue &PtrVal, Environment &Env) {
  Value *Prop = PtrVal.getProperty(IsNullProp);
  if (auto *BoolVal = dyn_cast_or_null<BoolValue>(Prop))
    return BoolVal;
  auto &NewVal = Env.makeAtomicBoolValue();
  PtrVal.setProperty(IsNullProp, NewVal);
  return &NewVal;
}

// Get nullness property for a pointer value (returns nullptr if not set)
BoolValue *getNullness(const PointerValue &PtrVal) {
  return dyn_cast_or_null<BoolValue>(PtrVal.getProperty(IsNullProp));
}

// Check if a pointer type is a smart pointer (unique_ptr, shared_ptr, weak_ptr)
bool isSmartPointer(const QualType &Ty) {
  if (!Ty->isRecordType())
    return false;
  const CXXRecordDecl *RD = Ty->getAsCXXRecordDecl();
  if (!RD || !RD->getIdentifier())
    return false;
  StringRef Name = RD->getName();
  if (Name != "unique_ptr" && Name != "shared_ptr" && Name != "weak_ptr")
    return false;
  return RD->getDeclContext()->isStdNamespace();
}

// Check if a CXXRecordDecl is a known smart pointer type
bool isSmartPointerClass(const CXXRecordDecl *RD) {
  if (!RD || !RD->getIdentifier())
    return false;
  StringRef Name = RD->getName();
  if (Name != "unique_ptr" && Name != "shared_ptr" && Name != "weak_ptr")
    return false;
  return RD->getDeclContext()->isStdNamespace();
}

// Check if a function is known to return a non-null pointer
bool returnsNonNull(const FunctionDecl *FD) {
  if (!FD)
    return false;

  // Check for _Nonnull or __nonnull attributes
  if (FD->hasAttr<NonNullAttr>())
    return true;

  // Check return type annotation
  QualType RetTy = FD->getReturnType();
  if (RetTy->hasAttr(attr::Nonnull))
    return true;

  // Known functions that return non-null
  if (FD->getIdentifier()) {
    StringRef Name = FD->getName();
    // std::make_unique, std::make_shared, std::addressof
    if (Name == "make_unique" || Name == "make_shared" || Name == "addressof") {
      if (FD->getDeclContext()->isStdNamespace())
        return true;
    }
  }

  return false;
}

// Check if an expression is known to produce a non-null pointer
bool exprProducesNonNull(const Expr *E) {
  if (!E)
    return false;

  E = E->IgnoreParenImpCasts();

  // new expressions always return non-null
  if (isa<CXXNewExpr>(E))
    return true;

  // std::addressof always returns non-null
  if (const auto *CE = dyn_cast<CallExpr>(E)) {
    if (const auto *FD = CE->getDirectCallee()) {
      if (returnsNonNull(FD))
        return true;
    }
  }

  return false;
}

// Check if an expression is known to be null (nullptr literal or 0)
bool isKnownNullExpr(const Expr *E) {
  if (!E)
    return false;

  E = E->IgnoreParenImpCasts();

  // nullptr literal
  if (isa<CXXNullPtrLiteralExpr>(E))
    return true;

  // integer literal 0 converted to pointer
  if (const auto *IL = dyn_cast<IntegerLiteral>(E)) {
    if (IL->getValue().isZero())
      return true;
  }

  // Implicit cast from null to pointer
  if (const auto *ICE = dyn_cast<ImplicitCastExpr>(E)) {
    if (ICE->getCastKind() == CK_NullToPointer)
      return true;
  }

  return false;
}

using LatticeTransferState = dataflow::TransferState<dataflow::NoopLattice>;

// Transfer function for pointer initialization
void transferDeclStmt(const DeclStmt *DS, LatticeTransferState &State) {
  Environment &Env = State.Env;

  for (const Decl *D : DS->decls()) {
    if (const auto *VD = dyn_cast<VarDecl>(D)) {
      QualType Ty = VD->getType();

      // Handle pointer variables
      if (Ty->isPointerType() || isSmartPointer(Ty)) {
        const Expr *Init = VD->getInit();
        if (!Init)
          continue;

        StorageLocation *Loc = Env.getStorageLocation(*VD);
        if (!Loc) {
          Loc = &Env.createStorageLocation(*VD);
          Env.setStorageLocation(*VD, *Loc);
        }

        if (Ty->isPointerType()) {
          // Raw pointer
          Value *PtrVal = Env.getValue(*Init);
          if (!PtrVal) {
            PtrVal = &Env.create<PointerValue>(*Loc);
            Env.setValue(*Loc, *PtrVal);
          }

          BoolValue *Nullness = getOrCreateNullness(*cast<PointerValue>(PtrVal), Env);

          // Set nullness based on initialization
          if (isKnownNullExpr(Init)) {
            // Definitely null
            Env.setValue(*Loc, *PtrVal);
            Env.assume(Env.arena().makeAtomRef(Nullness->getAtom()));
          } else if (exprProducesNonNull(Init)) {
            // Definitely non-null
            Env.setValue(*Loc, *PtrVal);
            Env.assume(Env.arena().makeNot(Nullness->formula()));
          } else {
            // Unknown - could be null
            Env.setValue(*Loc, *PtrVal);
            // Leave as atomic bool (unknown)
          }
        } else if (isSmartPointer(Ty)) {
          // Smart pointer - track its internal pointer nullness
          if (auto *RecordLoc = cast_or_null<dataflow::RecordStorageLocation>(Loc)) {
            BoolValue *Nullness = &Env.makeAtomicBoolValue();
            if (isKnownNullExpr(Init)) {
              Env.assume(Nullness->formula());
            } else if (exprProducesNonNull(Init)) {
              Env.assume(Env.arena().makeNot(Nullness->formula()));
            }
            // Store nullness as a synthetic field
            Env.setValue(RecordLoc->getSyntheticField("has_value"), *Nullness);
          }
        }
      }
    }
  }
}

// Transfer function for binary operators (assignment and comparison)
void transferBinaryOperator(const BinaryOperator *BO, LatticeTransferState &State) {
  Environment &Env = State.Env;

  if (BO->isAssignmentOp()) {
    const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
    const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

    // Pointer assignment
    if (LHS->getType()->isPointerType()) {
      StorageLocation *Loc = Env.getStorageLocation(*LHS);
      if (!Loc)
        return;

      Value *PtrVal = Env.getValue(*Loc);
      if (!PtrVal) {
        PtrVal = &Env.create<PointerValue>(*Loc);
        Env.setValue(*Loc, *PtrVal);
      }

      BoolValue *Nullness = getOrCreateNullness(*cast<PointerValue>(PtrVal), Env);

      if (isKnownNullExpr(RHS)) {
        Env.assume(Nullness->formula());
      } else if (exprProducesNonNull(RHS)) {
        Env.assume(Env.arena().makeNot(Nullness->formula()));
      }
    }
  }
}

// Transfer function for pointer comparisons in conditions
void transferBranchCondition(const Expr *Cond, bool Branch, Environment &Env) {
  auto &A = Env.arena();

  // Handle comparisons like: p != nullptr, p == nullptr, if (p), if (!p)
  Cond = Cond->IgnoreParenImpCasts();

  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (BO->isEqualityOp() || BO->isRelationalOp()) {
      const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
      const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

      // Check if one side is a pointer and the other is null
      bool LHSIsPtr = LHS->getType()->isPointerType();
      bool RHSIsPtr = RHS->getType()->isPointerType();
      bool LHSIsNull = isKnownNullExpr(LHS);
      bool RHSIsNull = isKnownNullExpr(RHS);

      if (LHSIsPtr && RHSIsNull) {
        StorageLocation *Loc = Env.getStorageLocation(*LHS);
        if (!Loc)
          return;
        Value *PtrVal = Env.getValue(*Loc);
        if (!PtrVal)
          return;
        BoolValue *Nullness = getOrCreateNullness(*cast<PointerValue>(PtrVal), Env);

        // p == nullptr: if Branch is true, p is null; if false, p is non-null
        // p != nullptr: if Branch is true, p is non-null; if false, p is null
        bool IsEq = BO->getOpcode() == BO_EQ;
        bool ShouldBeNull = IsEq ? Branch : !Branch;

        if (ShouldBeNull) {
          Env.assume(Nullness->formula());
        } else {
          Env.assume(A.makeNot(Nullness->formula()));
        }
      } else if (RHSIsPtr && LHSIsNull) {
        StorageLocation *Loc = Env.getStorageLocation(*RHS);
        if (!Loc)
          return;
        Value *PtrVal = Env.getValue(*Loc);
        if (!PtrVal)
          return;
        BoolValue *Nullness = getOrCreateNullness(*cast<PointerValue>(PtrVal), Env);

        bool IsEq = BO->getOpcode() == BO_EQ;
        bool ShouldBeNull = IsEq ? Branch : !Branch;

        if (ShouldBeNull) {
          Env.assume(Nullness->formula());
        } else {
          Env.assume(A.makeNot(Nullness->formula()));
        }
      }
    }
  } else if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() == UO_LNot) {
      // !p: if Branch is true, p is null; if false, p is non-null
      const Expr *SubExpr = UO->getSubExpr()->IgnoreParenImpCasts();
      if (SubExpr->getType()->isPointerType()) {
        StorageLocation *Loc = Env.getStorageLocation(*SubExpr);
        if (!Loc)
          return;
        Value *PtrVal = Env.getValue(*Loc);
        if (!PtrVal)
          return;
        BoolValue *Nullness = getOrCreateNullness(*cast<PointerValue>(PtrVal), Env);

        if (Branch) {
          Env.assume(Nullness->formula());
        } else {
          Env.assume(A.makeNot(Nullness->formula()));
        }
      }
    }
  } else if (Cond->getType()->isPointerType()) {
    // if (p): p is used as a boolean - if true (Branch), p is non-null
    StorageLocation *Loc = Env.getStorageLocation(*Cond);
    if (!Loc)
      return;
    Value *PtrVal = Env.getValue(*Loc);
    if (!PtrVal)
      return;
    BoolValue *Nullness = getOrCreateNullness(*cast<PointerValue>(PtrVal), Env);

    // if (p) means p != nullptr
    if (Branch) {
      Env.assume(A.makeNot(Nullness->formula()));
    } else {
      Env.assume(Nullness->formula());
    }
  }
}

// Build the transfer match switch
auto buildTransferMatchSwitch() {
  return dataflow::CFGMatchSwitchBuilder<LatticeTransferState>()
      .CaseOfCFGStmt<DeclStmt>(declStmt(), transferDeclStmt)
      .CaseOfCFGStmt<BinaryOperator>(binaryOperator(), transferBinaryOperator)
      .Build();
}

// Model for potential null pointer dereference analysis
class PotentialNullPtrDerefModel
    : public dataflow::DataflowAnalysis<PotentialNullPtrDerefModel, dataflow::NoopLattice> {
public:
  PotentialNullPtrDerefModel(ASTContext &Ctx, Environment &Env)
      : DataflowAnalysis(Ctx, Env),
        TransferMatchSwitch(buildTransferMatchSwitch()) {
    // Set up synthetic field callback for smart pointers
    Env.getDataflowAnalysisContext().setSyntheticFieldCallback(
        [&Ctx](QualType Ty) -> llvm::StringMap<QualType> {
          if (isSmartPointer(Ty)) {
            return {"has_value", Ctx.BoolTy};
          }
          return {};
        });
  }

  static dataflow::NoopLattice initialElement() { return {}; }

  void transfer(const CFGElement &Elt, dataflow::NoopLattice &L, Environment &Env) {
    LatticeTransferState State(L, Env);
    TransferMatchSwitch(Elt, getASTContext(), State);
  }

  void transferBranch(bool Branch, const Stmt *Stmt, dataflow::NoopLattice &L,
                      Environment &Env) {
    // Handle branch conditions for null pointer tracking
    if (const auto *If = dyn_cast<IfStmt>(Stmt)) {
      transferBranchCondition(If->getCond(), Branch, Env);
    } else if (const auto *While = dyn_cast<WhileStmt>(Stmt)) {
      transferBranchCondition(While->getCond(), Branch, Env);
    } else if (const auto *For = dyn_cast<ForStmt>(Stmt)) {
      if (For->getCond())
        transferBranchCondition(For->getCond(), Branch, Env);
    } else if (const auto *CondOp = dyn_cast<ConditionalOperator>(Stmt)) {
      transferBranchCondition(CondOp->getCond(), Branch, Env);
    }
  }

private:
  dataflow::CFGMatchSwitch<LatticeTransferState> TransferMatchSwitch;
};

// Diagnoser for potential null pointer dereferences
class PotentialNullPtrDerefDiagnoser {
public:
  llvm::SmallVector<SourceLocation>
  operator()(const CFGElement &Elt, ASTContext &Ctx,
             const TransferStateForDiagnostics<dataflow::NoopLattice> &State) {
    return DiagnoseMatchSwitch(Elt, Ctx, State.Env);
  }

private:
  // Check if dereference of a pointer is safe
  llvm::SmallVector<SourceLocation> checkDereference(const Expr *PtrExpr,
                                                     const Environment &Env) {
    if (!PtrExpr)
      return {};

    PtrExpr = PtrExpr->IgnoreParenImpCasts();

    // Get the pointer value
    StorageLocation *Loc = Env.getStorageLocation(*PtrExpr);
    if (!Loc)
      return {PtrExpr->getExprLoc()}; // Unknown pointer - could be null

    Value *Val = Env.getValue(*Loc);
    if (!Val)
      return {PtrExpr->getExprLoc()};

    if (auto *PtrVal = dyn_cast<PointerValue>(Val)) {
      BoolValue *Nullness = getNullness(*PtrVal);
      if (!Nullness) {
        // No nullness tracked - assume could be null
        return {PtrExpr->getExprLoc()};
      }

      // Check if we can prove the pointer is non-null
      auto &A = Env.arena();
      if (Env.proves(A.makeNot(Nullness->formula()))) {
        // Proven non-null - safe
        return {};
      }

      // Cannot prove non-null - potential null dereference
      return {PtrExpr->getExprLoc()};
    }

    return {};
  }

  dataflow::CFGMatchSwitch<const Environment, llvm::SmallVector<SourceLocation>>
      DiagnoseMatchSwitch = dataflow::CFGMatchSwitchBuilder<const Environment,
                                                            llvm::SmallVector<SourceLocation>>()
      // Detect -> operator (MemberExpr with arrow)
      .CaseOfCFGStmt<MemberExpr>(
          memberExpr(isArrow()),
          [this](const MemberExpr *ME, const MatchFinder::MatchResult &,
                 const Environment &Env) {
            return checkDereference(ME->getBase(), Env);
          })
      // Detect * operator (UnaryOperator dereference)
      .CaseOfCFGStmt<UnaryOperator>(
          unaryOperator(hasOperatorName("*")),
          [this](const UnaryOperator *UO, const MatchFinder::MatchResult &,
                 const Environment &Env) {
            return checkDereference(UO->getSubExpr(), Env);
          })
      // Detect smart pointer operator->
      .CaseOfCFGStmt<CXXOperatorCallExpr>(
          cxxOperatorCallExpr(hasOverloadedOperatorName("->")),
          [this](const CXXOperatorCallExpr *CE, const MatchFinder::MatchResult &,
                 const Environment &Env) {
            if (CE->getNumArgs() > 0) {
              const Expr *Arg = CE->getArg(0);
              if (isSmartPointer(Arg->getType())) {
                // Check smart pointer nullness
                StorageLocation *Loc = Env.getStorageLocation(*Arg);
                if (auto *RecordLoc = cast_or_null<dataflow::RecordStorageLocation>(Loc)) {
                  Value *HasVal = Env.getValue(RecordLoc->getSyntheticField("has_value"));
                  if (auto *BoolVal = dyn_cast_or_null<BoolValue>(HasVal)) {
                    if (Env.proves(Env.arena().makeNot(BoolVal->formula())))
                      return {};
                  }
                }
                return {Arg->getExprLoc()};
              }
              return checkDereference(Arg, Env);
            }
            return {};
          })
      // Detect smart pointer operator*
      .CaseOfCFGStmt<CXXOperatorCallExpr>(
          cxxOperatorCallExpr(hasOverloadedOperatorName("*")),
          [this](const CXXOperatorCallExpr *CE, const MatchFinder::MatchResult &,
                 const Environment &Env) {
            if (CE->getNumArgs() > 0) {
              const Expr *Arg = CE->getArg(0);
              if (isSmartPointer(Arg->getType())) {
                StorageLocation *Loc = Env.getStorageLocation(*Arg);
                if (auto *RecordLoc = cast_or_null<dataflow::RecordStorageLocation>(Loc)) {
                  Value *HasVal = Env.getValue(RecordLoc->getSyntheticField("has_value"));
                  if (auto *BoolVal = dyn_cast_or_null<BoolValue>(HasVal)) {
                    if (Env.proves(Env.arena().makeNot(BoolVal->formula())))
                      return {};
                  }
                }
                return {Arg->getExprLoc()};
              }
              return checkDereference(Arg, Env);
            }
            return {};
          })
      .Build();
};

} // anonymous namespace

void PotentialNullPtrDerefCheck::registerMatchers(MatchFinder *Finder) {
  // Match functions that may contain pointer dereferences
  Finder->addMatcher(
      functionDecl(unless(isExpansionInSystemHeader()),
                   hasBody(stmt(hasDescendant(
                       anyOf(memberExpr(isArrow()),
                             unaryOperator(hasOperatorName("*")),
                             cxxOperatorCallExpr(hasAnyOverloadedOperatorName("->", "*"))))))
                   )
          .bind(FuncID),
      this);
}

void PotentialNullPtrDerefCheck::check(const MatchFinder::MatchResult &Result) {
  if (Result.SourceManager->getDiagnostics().hasUncompilableErrorOccurred())
    return;

  const auto *FuncDecl = Result.Nodes.getNodeAs<FunctionDecl>(FuncID);
  if (!FuncDecl || !FuncDecl->getBody())
    return;

  // Skip templated functions
  if (FuncDecl->isTemplated())
    return;

  PotentialNullPtrDerefDiagnoser Diagnoser;

  auto Diags = dataflow::diagnoseFunction<PotentialNullPtrDerefModel, SourceLocation>(
      *FuncDecl, *Result.Context, Diagnoser);

  if (auto Err = Diags.takeError()) {
    llvm::consumeError(std::move(Err));
    return;
  }

  for (const SourceLocation &Loc : *Diags) {
    diag(Loc, "potential null pointer dereference; pointer may be null at this point");
  }
}

} // namespace clang::tidy::haha