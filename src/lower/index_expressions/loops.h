#ifndef SIMIT_LOOPS_H
#define SIMIT_LOOPS_H

#include <vector>
#include <ostream>

#include "ir.h"
#include "tensor_index.h"

namespace simit {
namespace ir {

/// An index variable loop is the loop associated with a (free or reduction)
/// index variable. An index variable loop can be linked to another index
/// variable loop, which means that only some of the index variable values need
/// to be traversed, as determined by tensor indices.
class IndexVariableLoop {
public:
  IndexVariableLoop();
  IndexVariableLoop(const IndexVar& indexVar);
  IndexVariableLoop(const IndexVar& indexVar, IndexVariableLoop linkedLoop);

  const IndexVar& getIndexVar() const;
  const Var& getInductionVar() const;

  bool isLinked() const;
  const IndexVariableLoop& getLinkedLoop() const;

  bool defined() const {return content != nullptr;}

private:
  struct Content;
  std::shared_ptr<Content> content;
};

/// A TensorIndexVar is a pair of loop induction variables, a coordinate
/// variable and a sink variable, that are retrieved from a tensor index using a
/// source variable. That is, the mapping:
///     (tensorIndex, sourceVar) -> (coordinateVar, sinkVar).
///
/// For example, (A.row2col, i) -> (ijA, jA) is evaluated as follows:
///     ijA = A.row2col.sources[i];
///      jA = A.row2col.sinks[ijA];
///
/// Given the expression c=A*b, ijA can be used to retrieve the matrix component
/// at location (i,j) in A, while i can index into c and jA into b. For example,
///     c[i] += A[ijA] * b[jA];
///
/// When merging multiple loops over different tensor index variables, their
/// sink variables are merged into the overall loop induction variable. For
/// example, jA and jB are merged into j.
class TensorIndexVar {
public:
  TensorIndexVar(std::string inductionVarName, std::string tensorName,
                 Var sourceVar, TensorIndex tensorIndex);

  const Var& getSourceVar() const {return sourceVar;}
  const Var& getCoordinateVar() const {return coordinateVar;}
  const Var& getSinkVar() const {return sinkVar;}

  const TensorIndex& getTensorIndex() const {return tensorIndex;}

  Expr loadCoordinate(int offset=0) const;
  Expr loadSink() const;
  Stmt initCoordinateVar() const;
  Stmt initSinkVar() const;
  Stmt initSinkVar(const Var& sinkVar) const;

  friend std::ostream& operator<<(std::ostream&, const TensorIndexVar&);

private:
  Var sourceVar;
  Var coordinateVar;
  Var sinkVar;
  TensorIndex tensorIndex;
};

class SubsetLoop {
public:
  SubsetLoop(const std::vector<TensorIndexVar>& tensorIndexVars,
             Expr computeExpr, Expr indexExpr)
      : tensorIndexVars(tensorIndexVars),
        computeExpr(computeExpr), indexExpr(indexExpr) {}

  void setCompoundOperator(CompoundOperator cop) {this->cop = cop;}

  const std::vector<TensorIndexVar>& getTensorIndexVars() const {
    return tensorIndexVars;
  }

  CompoundOperator getCompoundOperator() const {return cop;}

  const Expr& getComputeExpression() const {return computeExpr;}

  const Expr& getIndexExpression() const {return indexExpr;}

  friend std::ostream& operator<<(std::ostream&, const SubsetLoop&);

private:
  std::vector<TensorIndexVar> tensorIndexVars;
  CompoundOperator cop = CompoundOperator::None;

  Expr computeExpr;
  Expr indexExpr;
};


std::vector<SubsetLoop> createSubsetLoops(const IndexExpr* indexExpression,
                                          IndexVariableLoop loop,
                                          Environment* env);

}}
#endif
