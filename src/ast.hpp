#ifndef __AST_HPP_
#define __AST_HPP_

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/ExecutionEngine/Orc/Core.h"

using namespace llvm;
using namespace llvm::orc;

class PrototypeAST;
extern std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;

class ExprAST {
 public:
  virtual ~ExprAST() = default;
  virtual Value *codegen() = 0;
};

typedef std::unique_ptr<ExprAST> BaseASTPtr;

class VariableExprAST : public ExprAST {
  std::string Name;

 public:
  VariableExprAST(const std::string &name) : Name(name) {}
  const std::string& getName() const { return Name; }
  Value *codegen() override;
};

class NumberExprAST : public ExprAST {
  double val_;

 public:
  NumberExprAST(double val) : val_(val) {}
  Value *codegen() override;
};

class UnaryExprAST : public ExprAST {
  char Opcode;
  std::unique_ptr<ExprAST> Operand;
public:
  UnaryExprAST(char Opcode, std::unique_ptr<ExprAST> Operand)
    : Opcode(Opcode), Operand(std::move(Operand)) {}
  Value *codegen() override;
};

class BinaryExprAST : public ExprAST {
  char Op;
  BaseASTPtr LHS, RHS;

 public:
  BinaryExprAST(const char op, BaseASTPtr lhs, BaseASTPtr rhs)
      : Op(op), LHS(std::move(lhs)), RHS(std::move(rhs)){};
  Value *codegen() override;
};

class VarExprAST : public ExprAST {
  using VarMap = std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>>;
  VarMap VarNames;
  std::unique_ptr<ExprAST> Body;

public:
  VarExprAST(VarMap VarNames, std::unique_ptr<ExprAST> Body)
      : VarNames(std::move(VarNames)), Body(std::move(Body)) {}
  Value *codegen() override;
};

class PrototypeAST {
  std::string Name;
  std::vector<std::string> args_;
  bool IsOperator;
  unsigned Precedence;

 public:
  PrototypeAST(const std::string &name, const std::vector<std::string> &args,
               bool IsOperator = false, unsigned Prec = 0)
      : Name(name), args_(args), IsOperator(IsOperator), Precedence(Prec) {};
  const std::string &getName() const { return Name; }
  Function *codegen();

  bool isUnaryOp() const { return IsOperator && args_.size() == 1; }
  bool isBinaryOp() const { return IsOperator && args_.size() == 2; }

  char getOperatorName() const {
    assert(isUnaryOp() || isBinaryOp());
    return Name[Name.size() - 1];
  }

  unsigned getBinaryPrecedence() const { return Precedence; }
};

class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  BaseASTPtr Body;

 public:
  FunctionAST(std::unique_ptr<PrototypeAST> proto, BaseASTPtr body)
      : Proto(std::move(proto)), Body(std::move(body)) {}
  const PrototypeAST& getProto() const { return *Proto; }
  const std::string& getName() const { return Proto->getName(); }
  Function *codegen();
};

class CallExprAST : public ExprAST {
  std::string callee_;
  std::vector<BaseASTPtr> args_;

 public:
  CallExprAST(const std::string &callee, std::vector<BaseASTPtr> args)
      : callee_(callee), args_(std::move(args)) {}
  Value *codegen() override;
};

class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;

public:
  IfExprAST(std::unique_ptr<ExprAST> Cond, std::unique_ptr<ExprAST> Then,
            std::unique_ptr<ExprAST> Else)
    : Cond(std::move(Cond)), Then(std::move(Then)), Else(std::move(Else)) {}
  Value *codegen() override;
};

class ForExprAST : public ExprAST {
  std::string VarName;
  std::unique_ptr<ExprAST> Start, End, Step, Body;

public:
  ForExprAST(const std::string& VarName, std::unique_ptr<ExprAST> Start,
             std::unique_ptr<ExprAST> End, std::unique_ptr<ExprAST> Step,
             std::unique_ptr<ExprAST> Body)
    : VarName(VarName), Start(std::move(Start)), End(std::move(End)),
      Step(std::move(Step)), Body(std::move(Body)) {}

  Value *codegen() override;
};

BaseASTPtr LogError(const char *str);
std::unique_ptr<PrototypeAST> LogErrorP(const char *str);
Value *LogErrorV(const char *str);

#endif