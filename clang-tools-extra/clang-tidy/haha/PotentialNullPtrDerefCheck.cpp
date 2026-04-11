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
using dataflow::BoolValue;
using dataflow::Environment;
using dataflow::PointerValue;
using dataflow::StorageLocation;
using dataflow::TransferStateForDiagnostics;
using dataflow::Value;

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
  if (RetTy->hasAttr(attr::NonNull))
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
void transferDeclStmt(const DeclStmt *DS,
                      const MatchFinder::MatchResult &,
                      LatticeTransferState &State) {
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
          // Check if we already have a PointerValue - don't create new one
          Value *ExistingVal = Env.getValue(*Loc);
          PointerValue *PtrVal = nullptr;

          if (ExistingVal) {
            PtrVal = dyn_cast<PointerValue>(ExistingVal);
          }

          if (!PtrVal) {
            // Create new PointerValue only if none exists
            StorageLocation &PointeeLoc = Env.createStorageLocation(Ty->getPointeeType());
            PtrVal = &Env.create<PointerValue>(PointeeLoc);
            Env.setValue(*Loc, *PtrVal);
          }

          BoolValue *Nullness = getOrCreateNullness(*PtrVal, Env);

          // Set nullness based on initialization
          if (isKnownNullExpr(Init)) {
            // Definitely null - assume nullness is true
            Env.assume(Nullness->formula());
          } else if (exprProducesNonNull(Init)) {
            // Definitely non-null - assume nullness is false
            Env.assume(Env.arena().makeNot(Nullness->formula()));
          }
          // Unknown initialization - leave nullness unconstrained (could be null or non-null)
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
void transferBinaryOperator(const BinaryOperator *BO,
                            const MatchFinder::MatchResult &,
                            LatticeTransferState &State) {
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


// Transfer function for function calls - reset pointer nullness when passed by reference
void transferCallExpr(const CallExpr *CE, const MatchFinder::MatchResult &,
                      LatticeTransferState &State) {
  Environment &Env = State.Env;

  // Check if any argument is a pointer passed by reference (pointer to pointer)
  // In such cases, we cannot know if the function modifies the pointer
  for (unsigned I = 0; I < CE->getNumArgs(); ++I) {
    const Expr *Arg = CE->getArg(I)->IgnoreParenImpCasts();

    // Check if argument is a pointer to pointer (e.g., Info**)
    if (Arg->getType()->isPointerType()) {
      QualType PointeeType = Arg->getType()->getPointeeType();
      if (PointeeType->isPointerType()) {
        // This is a pointer-to-pointer argument (e.g., &pInfo)
        // If the inner pointer is a DeclRefExpr, we need to reset its nullness
        if (const auto *UO = dyn_cast<UnaryOperator>(Arg)) {
          if (UO->getOpcode() == UO_AddrOf) {
            const Expr *SubExpr = UO->getSubExpr()->IgnoreParenImpCasts();
            if (const auto *DRE = dyn_cast<DeclRefExpr>(SubExpr)) {
              if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
                if (VD->getType()->isPointerType()) {
                  // Reset the nullness of the pointer variable to unknown
                  StorageLocation *Loc = Env.getStorageLocation(*VD);
                  if (Loc) {
                    Value *Val = Env.getValue(*Loc);
                    if (auto *PtrVal = dyn_cast_or_null<PointerValue>(Val)) {
                      // Create a fresh unknown nullness
                      BoolValue &NewNullness = Env.makeAtomicBoolValue();
                      PtrVal->setProperty(IsNullProp, NewNullness);
                      // Don't assume anything about it - it's now unknown
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }
}

// Helper function to get pointer nullness from an expression
// Returns the nullness BoolValue if the expression is a pointer, nullptr otherwise
BoolValue *getPointerNullnessFromExpr(const Expr *E, Environment &Env) {
  E = E->IgnoreParenImpCasts();

  if (!E->getType()->isPointerType())
    return nullptr;

  // For DeclRefExpr, get the value directly from the variable's storage location
  if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
    if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
      StorageLocation *Loc = Env.getStorageLocation(*VD);
      if (!Loc)
        return nullptr;
      Value *Val = Env.getValue(*Loc);
      if (!Val)
        return nullptr;
      if (auto *PtrVal = dyn_cast<PointerValue>(Val))
        return getOrCreateNullness(*PtrVal, Env);
    }
    return nullptr;
  }

  // Try to get PointerValue directly from expression
  auto *PtrVal = Env.get<PointerValue>(*E);
  if (PtrVal)
    return getOrCreateNullness(*PtrVal, Env);

  // Try to get from storage location
  StorageLocation *Loc = Env.getStorageLocation(*E);
  if (!Loc)
    return nullptr;
  Value *Val = Env.getValue(*Loc);
  if (!Val)
    return nullptr;
  PtrVal = dyn_cast<PointerValue>(Val);
  if (!PtrVal)
    return nullptr;

  return getOrCreateNullness(*PtrVal, Env);
}

// Transfer function for pointer-to-bool conversion
// This is crucial for making branch conditions work correctly
// When a pointer is used in a boolean context (if (p), while (p), etc.),
// we need to connect the BoolValue to the pointer's nullness property
void transferPointerToBoolean(const ImplicitCastExpr *ICE,
                              const MatchFinder::MatchResult &,
                              LatticeTransferState &State) {
  Environment &Env = State.Env;

  const Expr *SubExpr = ICE->getSubExpr();
  if (!SubExpr)
    return;

  // Get the pointer value from the subexpression
  // The subexpression should be a PointerValue (from LValueToRValue cast or similar)
  auto *PtrVal = Env.get<PointerValue>(*SubExpr);
  if (!PtrVal) {
    // Try to get from storage location
    StorageLocation *Loc = Env.getStorageLocation(*SubExpr);
    if (!Loc)
      return;
    Value *Val = Env.getValue(*Loc);
    if (!Val)
      return;
    PtrVal = dyn_cast<PointerValue>(Val);
    if (!PtrVal)
      return;
  }

  // Get or create the nullness property for this pointer
  BoolValue *Nullness = getOrCreateNullness(*PtrVal, Env);

  // Set the nullness BoolValue as the value of this cast expression
  // This connects pointer nullness to the framework's branch condition handling
  Env.setValue(*ICE, *Nullness);
}

// Transfer function for pointer equality comparisons with null
// Handles p == nullptr and p != nullptr
void transferPointerNullComparison(const BinaryOperator *BO,
                                   const MatchFinder::MatchResult &,
                                   LatticeTransferState &State) {
  Environment &Env = State.Env;

  if (!BO->isEqualityOp())
    return;

  const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
  const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

  // Check if one side is a pointer and the other is null
  bool LHSIsPtr = LHS->getType()->isPointerType();
  bool RHSIsPtr = RHS->getType()->isPointerType();
  bool LHSIsNull = isKnownNullExpr(LHS);
  bool RHSIsNull = isKnownNullExpr(RHS);

  BoolValue *Nullness = nullptr;

  if (LHSIsPtr && RHSIsNull) {
    // p == nullptr or p != nullptr
    auto *PtrVal = Env.get<PointerValue>(*LHS);
    if (!PtrVal) {
      StorageLocation *Loc = Env.getStorageLocation(*LHS);
      if (Loc) {
        Value *Val = Env.getValue(*Loc);
        PtrVal = dyn_cast_or_null<PointerValue>(Val);
      }
    }
    if (PtrVal)
      Nullness = getOrCreateNullness(*PtrVal, Env);
  } else if (RHSIsPtr && LHSIsNull) {
    // nullptr == p or nullptr != p
    auto *PtrVal = Env.get<PointerValue>(*RHS);
    if (!PtrVal) {
      StorageLocation *Loc = Env.getStorageLocation(*RHS);
      if (Loc) {
        Value *Val = Env.getValue(*Loc);
        PtrVal = dyn_cast_or_null<PointerValue>(Val);
      }
    }
    if (PtrVal)
      Nullness = getOrCreateNullness(*PtrVal, Env);
  }

  if (!Nullness)
    return;

  // p == nullptr: result is Nullness (true if null)
  // p != nullptr: result is !Nullness (true if not null)
  if (BO->getOpcode() == BO_EQ) {
    Env.setValue(*BO, *Nullness);
  } else { // BO_NE
    Env.setValue(*BO, Env.makeNot(*Nullness));
  }
}

// Build the transfer match switch
auto buildTransferMatchSwitch() {
  using namespace ast_matchers;

  // Matcher for pointer-to-boolean conversion (used in conditions like if (p))
  auto PointerToBoolMatcher = implicitCastExpr(hasCastKind(CK_PointerToBoolean));

  // Matcher for pointer-null equality comparison (p == nullptr, p != nullptr)
  // We need to match: pointer == null, null == pointer, pointer != null, null != pointer
  auto PointerNullEqMatcher = binaryOperator(
      hasAnyOperatorName("==", "!="),
      anyOf(
          // pointer == nullptr or pointer != nullptr
          allOf(hasLHS(hasType(pointerType())),
                hasRHS(ignoringParenImpCasts(nullPointerConstant()))),
          // nullptr == pointer or nullptr != pointer
          allOf(hasLHS(ignoringParenImpCasts(nullPointerConstant())),
                hasRHS(hasType(pointerType())))
      ));

  return dataflow::CFGMatchSwitchBuilder<LatticeTransferState>()
      .CaseOfCFGStmt<DeclStmt>(declStmt(), transferDeclStmt)
      .CaseOfCFGStmt<BinaryOperator>(binaryOperator(), transferBinaryOperator)
      .CaseOfCFGStmt<CallExpr>(callExpr(), transferCallExpr)
      // Handle pointer-to-boolean conversion - this is key for branch conditions
      .CaseOfCFGStmt<ImplicitCastExpr>(PointerToBoolMatcher, transferPointerToBoolean)
      // Handle pointer-null equality comparisons
      .CaseOfCFGStmt<BinaryOperator>(PointerNullEqMatcher, transferPointerNullComparison)
      .Build();
}

// Model for potential null pointer dereference analysis
class PotentialNullPtrDerefModel
    : public dataflow::DataflowAnalysis<PotentialNullPtrDerefModel, dataflow::NoopLattice> {
public:
  PotentialNullPtrDerefModel(ASTContext &Ctx, Environment &Env)
      : DataflowAnalysis(Ctx),
        TransferMatchSwitch(buildTransferMatchSwitch()) {
    // Set up synthetic field callback for smart pointers
    Env.getDataflowAnalysisContext().setSyntheticFieldCallback(
        [&Ctx](QualType Ty) -> llvm::StringMap<QualType> {
          if (isSmartPointer(Ty)) {
            llvm::StringMap<QualType> Fields;
            Fields["has_value"] = Ctx.BoolTy;
            return Fields;
          }
          return llvm::StringMap<QualType>();
        });
  }

  static dataflow::NoopLattice initialElement() { return {}; }

  void transfer(const CFGElement &Elt, dataflow::NoopLattice &L, Environment &Env) {
    LatticeTransferState State(L, Env);
    TransferMatchSwitch(Elt, getASTContext(), State);
  }

  // Handle branch conditions for pointer nullness tracking
  // This is called by the framework when evaluating branch conditions
  // We directly assume pointer nullness based on the branch direction
  // IMPORTANT: Stmt is the condition expression, not the terminator statement!
  void transferBranch(bool Branch, const Stmt *S, dataflow::NoopLattice &,
                      Environment &Env) {
    // S is already the condition expression (e.g., !pInfo, pInfo == nullptr, etc.)
    const Expr *Cond = dyn_cast<Expr>(S);
    if (!Cond)
      return;

    Cond = Cond->IgnoreParenImpCasts();
    auto &A = Env.arena();

    // Case 1: Pointer used directly as boolean (if (p))
    // p is true iff p != nullptr (i.e., p is not null)
    if (const auto *ICE = dyn_cast<ImplicitCastExpr>(Cond)) {
      if (ICE->getCastKind() == CK_PointerToBoolean) {
        BoolValue *Nullness = getPointerNullnessFromExpr(ICE->getSubExpr(), Env);
        if (Nullness) {
          // if (p) with Branch=true: p is non-null, assume !Nullness
          // if (p) with Branch=false: p is null, assume Nullness
          if (Branch) {
            Env.assume(A.makeNot(Nullness->formula()));
          } else {
            Env.assume(Nullness->formula());
          }
        }
      }
    }

    // Case 2: Pointer negation (if (!p))
    // !p is true iff p is null
    if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
      if (UO->getOpcode() == UO_LNot) {
        const Expr *SubExpr = UO->getSubExpr()->IgnoreParenImpCasts();
        BoolValue *Nullness = getPointerNullnessFromExpr(SubExpr, Env);
        if (Nullness) {
          // if (!p) with Branch=true: p is null, assume Nullness
          // if (!p) with Branch=false: p is non-null, assume !Nullness
          if (Branch) {
            Env.assume(Nullness->formula());
          } else {
            Env.assume(A.makeNot(Nullness->formula()));
          }
        }
      }
    }

    // Case 3: Pointer compared to null (if (p == nullptr), if (p != nullptr))
    if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
      if (BO->isEqualityOp()) {
        const Expr *LHS = BO->getLHS()->IgnoreParenImpCasts();
        const Expr *RHS = BO->getRHS()->IgnoreParenImpCasts();

        BoolValue *Nullness = nullptr;

        if (LHS->getType()->isPointerType() && isKnownNullExpr(RHS)) {
          Nullness = getPointerNullnessFromExpr(LHS, Env);
        } else if (RHS->getType()->isPointerType() && isKnownNullExpr(LHS)) {
          Nullness = getPointerNullnessFromExpr(RHS, Env);
        }

        if (Nullness) {
          // p == nullptr with Branch=true: p is null, assume Nullness
          // p == nullptr with Branch=false: p is non-null, assume !Nullness
          // p != nullptr with Branch=true: p is non-null, assume !Nullness
          // p != nullptr with Branch=false: p is null, assume Nullness
          if (BO->getOpcode() == BO_EQ) {
            if (Branch) {
              Env.assume(Nullness->formula());
            } else {
              Env.assume(A.makeNot(Nullness->formula()));
            }
          } else { // BO_NE
            if (Branch) {
              Env.assume(A.makeNot(Nullness->formula()));
            } else {
              Env.assume(Nullness->formula());
            }
          }
        }
      }
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
      return llvm::SmallVector<SourceLocation>();

    PtrExpr = PtrExpr->IgnoreParenImpCasts();

    // Get the pointer value
    StorageLocation *Loc = Env.getStorageLocation(*PtrExpr);
    if (!Loc)
      return llvm::SmallVector<SourceLocation>({PtrExpr->getExprLoc()}); // Unknown pointer - could be null

    Value *Val = Env.getValue(*Loc);
    if (!Val)
      return llvm::SmallVector<SourceLocation>({PtrExpr->getExprLoc()});

    if (auto *PtrVal = dyn_cast<PointerValue>(Val)) {
      BoolValue *Nullness = getNullness(*PtrVal);
      if (!Nullness) {
        // No nullness tracked - assume could be null
        return llvm::SmallVector<SourceLocation>({PtrExpr->getExprLoc()});
      }

      // Check if we can prove the pointer is non-null
      auto &A = Env.arena();
      if (Env.proves(A.makeNot(Nullness->formula()))) {
        // Proven non-null - safe
        return llvm::SmallVector<SourceLocation>();
      }

      // Cannot prove non-null - potential null dereference
      return llvm::SmallVector<SourceLocation>({PtrExpr->getExprLoc()});
    }

    return llvm::SmallVector<SourceLocation>();
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
                      return llvm::SmallVector<SourceLocation>();
                  }
                }
                return llvm::SmallVector<SourceLocation>({Arg->getExprLoc()});
              }
              return checkDereference(Arg, Env);
            }
            return llvm::SmallVector<SourceLocation>();
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
                      return llvm::SmallVector<SourceLocation>();
                  }
                }
                return llvm::SmallVector<SourceLocation>({Arg->getExprLoc()});
              }
              return checkDereference(Arg, Env);
            }
            return llvm::SmallVector<SourceLocation>();
          })
      .Build();
};

} // anonymous namespace

void PotentialNullPtrDerefCheck::registerMatchers(MatchFinder *Finder) {
  // Match functions that may contain pointer dereferences
  auto HasPtrDerefDescendant = hasDescendant(
      stmt(anyOf(memberExpr(isArrow()),
                 unaryOperator(hasOperatorName("*")),
                 cxxOperatorCallExpr(hasAnyOverloadedOperatorName("->", "*")))));

  Finder->addMatcher(
      functionDecl(unless(isExpansionInSystemHeader()),
                   hasBody(HasPtrDerefDescendant))
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