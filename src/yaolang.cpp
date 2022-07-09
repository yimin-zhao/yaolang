#include "ast.hpp"
#include "yaoyaoJIT.hpp"
#include "RemoteJITUtil.hpp"
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>

static double numeric_val;
static std::string identifier;
static char current_token;

LLVMContext TheContext;
IRBuilder<> Builder(TheContext);
std::unique_ptr<Module> TheModule;
std::map<std::string, AllocaInst *> NamedValues;
std::map<char, int> BinOpPrecedence;
std::unique_ptr<legacy::FunctionPassManager> TheFPM;
std::map<std::string, std::unique_ptr<PrototypeAST>> FunctionProtos;
std::unique_ptr<YaoyaoJIT> TheJIT;
ExitOnError ExitOnErr;

BaseASTPtr LogError(const char *str) {
  fprintf(stderr, "LogError: %s\n", str);
  return nullptr;
}

std::unique_ptr<PrototypeAST> LogErrorP(const char *str) {
  LogError(str);
  return nullptr;
}

Value *LogErrorV(const char *str) {
  LogError(str);
  return nullptr;
}

Function *getFunction(std::string Name) {
  if (auto *F = TheModule->getFunction(Name))
    return F;

  auto FI = FunctionProtos.find(Name);
  if (FI != FunctionProtos.end())
    return FI->second->codegen();

  return nullptr;
}

AllocaInst *CreateEntryBlockAlloca(Function *TheFunction, StringRef VarName) {
  IRBuilder<> TmpB(&TheFunction->getEntryBlock(),
                    TheFunction->getEntryBlock().begin());
  return TmpB.CreateAlloca(Type::getDoubleTy(TheContext), nullptr, VarName);
}

Value *NumberExprAST::codegen() {
  return ConstantFP::get(TheContext, llvm::APFloat(val_));
}

Value *VariableExprAST::codegen() {
  Value *v = NamedValues[Name];
  if (!v) LogErrorV("Unknow variable name");
  return Builder.CreateLoad(v, Name.c_str());
}

Value *UnaryExprAST::codegen() {
  Value *OperandV = Operand->codegen();
  if (!OperandV)
    return nullptr;

  Function *F = getFunction(std::string("unary") + Opcode);

  if (!F)
    return LogErrorV("Unknown unary operator");

  return Builder.CreateCall(F, OperandV, "unop");
}

Value *BinaryExprAST::codegen() {

  if (Op == '=') {
    VariableExprAST *LHSE = static_cast<VariableExprAST *>(LHS.get());
    if (!LHSE)
      return LogErrorV("destination of '=' must be a variable");

    Value *Val = RHS->codegen();
    if (!Val)
      return nullptr;

    Value *Variable = NamedValues[LHSE->getName()];
    if (!Variable)
      return LogErrorV("Unknown variable name");

    Builder.CreateStore(Val, Variable);
    return Val;
  }

  Value *l = LHS->codegen();
  Value *r = RHS->codegen();
  if (!l || !r) return nullptr;

  switch (Op) {
    case '+':
      return Builder.CreateFAdd(l, r, "Addtmp");
    case '-':
      return Builder.CreateFSub(l, r, "subtmp");
    case '*':
      return Builder.CreateFMul(l, r, "multmp");
    case '<':
      l = Builder.CreateFCmpULT(l, r, "cmptmp");
      return Builder.CreateUIToFP(l, Type::getDoubleTy(TheContext), "booltmp");
    default:
      return LogErrorV("invalid binary operator");
  }

  Function *F = getFunction(std::string("binary") + Op);
  assert(F && "binary operator not found");

  Value *Ops[] = {l, r};
  return Builder.CreateCall(F, Ops, "binop");
}

Value *CallExprAST::codegen() {
  llvm::Function *calleeF = getFunction(callee_);
  if (!calleeF) return LogErrorV("Unknown function referenced");

  if (calleeF->arg_size() != args_.size())
    return LogErrorV("Incorrect # arguemnts passed");

  std::vector<Value *> argsV;
  for (size_t i = 0, e = args_.size(); i != e; i++) {
    argsV.push_back(args_[i]->codegen());
    if (!argsV.back()) return nullptr;
  }

  return Builder.CreateCall(calleeF, argsV, "calltmp");
}

Value *IfExprAST::codegen() {
  Value *CondV = Cond->codegen();
  if (!CondV)
    return nullptr;

  CondV = Builder.CreateFCmpONE(CondV,
                    ConstantFP::get(TheContext, APFloat(0.0)), "ifcond");

  Function *TheFunction = Builder.GetInsertBlock()->getParent();

  BasicBlock *ThenBB = BasicBlock::Create(TheContext, "then", TheFunction);
  BasicBlock *ElseBB = BasicBlock::Create(TheContext, "else");
  BasicBlock *MergeBB = BasicBlock::Create(TheContext, "ifcont");

  Builder.CreateCondBr(CondV, ThenBB, ElseBB);
  Builder.SetInsertPoint(ThenBB);

  Value *ThenV = Then->codegen();
  if (!ThenV)
    return nullptr;

  Builder.CreateBr(MergeBB);
  ThenBB = Builder.GetInsertBlock();

  TheFunction->getBasicBlockList().push_back(ElseBB);
  Builder.SetInsertPoint(ElseBB);

  Value *ElseV = Else->codegen();
  if (!ElseV)
    return nullptr;

  Builder.CreateBr(MergeBB);
  ElseBB = Builder.GetInsertBlock();

  TheFunction->getBasicBlockList().push_back(MergeBB);
  Builder.SetInsertPoint(MergeBB);

  PHINode *PN = Builder.CreatePHI(Type::getDoubleTy(TheContext), 2, "iftmp");

  PN->addIncoming(ThenV, ThenBB);
  PN->addIncoming(ElseV, ElseBB);
  return PN;
}

Value *ForExprAST::codegen() {
  Function *TheFunction = Builder.GetInsertBlock()->getParent();
  AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);

  Value *StartVal = Start->codegen();
  if (!StartVal)
    return nullptr;

  Builder.CreateStore(StartVal, Alloca);

  BasicBlock *LoopBB = BasicBlock::Create(TheContext, "loop", TheFunction);

  Builder.CreateBr(LoopBB);
  Builder.SetInsertPoint(LoopBB);

  AllocaInst *OldVal = NamedValues[VarName];
  NamedValues[VarName] = Alloca;

  if (!Body->codegen())
    return nullptr;

  Value *StepVal = nullptr;
  if (Step) {
    StepVal = Step->codegen();
    if (!StepVal)
      return nullptr;
  } else {
    StepVal = ConstantFP::get(TheContext, APFloat(1.0));
  }

  Value *EndCond = End->codegen();
  if (!EndCond)
    return nullptr;

  Value *CurVar = Builder.CreateLoad(Alloca, VarName.c_str());
  Value *NextVar = Builder.CreateAdd(CurVar, StepVal, "nextvar");
  Builder.CreateStore(NextVar, Alloca);

  EndCond = Builder.CreateFCmpONE(EndCond,
                ConstantFP::get(TheContext, APFloat(0.0)), "loopcond");

  BasicBlock *AfterBB =
      BasicBlock::Create(TheContext, "afterloop", TheFunction);

  Builder.CreateCondBr(EndCond, LoopBB, AfterBB);

  Builder.SetInsertPoint(AfterBB);

  if (OldVal)
    NamedValues[VarName] = OldVal;
  else
    NamedValues.erase(VarName);

  return Constant::getNullValue(Type::getDoubleTy(TheContext));
}

Value *VarExprAST::codegen() {
  std::vector<AllocaInst *> OldBindings;

  Function *TheFunction = Builder.GetInsertBlock()->getParent();

  for (unsigned i = 0, e = VarNames.size(); i!= e; ++i) {
    const std::string &VarName = VarNames[i].first;
    ExprAST *Init = VarNames[i].second.get();

    Value *InitVal;
    if (Init) {
      InitVal = Init->codegen();
      if (!InitVal)
        return nullptr;
    } else {
      InitVal = ConstantFP::get(TheContext, APFloat(0.0));
    }

    AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, VarName);
    Builder.CreateStore(InitVal, Alloca);

    OldBindings.push_back(NamedValues[VarName]);

    NamedValues[VarName] = Alloca;
  }

  Value *BodyVal = Body->codegen();
  if (!BodyVal)
    return nullptr;

  for (unsigned i = 0, e = VarNames.size(); i != e; ++i)
    NamedValues[VarNames[i].first] = OldBindings[i];

  return BodyVal;
}

Function *PrototypeAST::codegen() {
  std::vector<Type *> doubles(args_.size(), Type::getDoubleTy(TheContext));

  FunctionType *ft =
      FunctionType::get(Type::getDoubleTy(TheContext), doubles, false);
  Function *f =
      Function::Create(ft, Function::ExternalLinkage, Name, TheModule.get());

  size_t idx = 0;
  for (auto &arg : f->args()) {
    arg.setName(args_[idx++]);
  }
  return f;
}

Function *FunctionAST::codegen() {
  auto &P = *Proto;

  Function *TheFunction = getFunction(P.getName());
  if (!TheFunction)
    return nullptr;

  if (P.isBinaryOp())
    BinOpPrecedence[P.getBinaryPrecedence()] = P.getBinaryPrecedence();

  BasicBlock *BB = BasicBlock::Create(TheContext, "entry", TheFunction);
  Builder.SetInsertPoint(BB);

  NamedValues.clear();

  for (auto &arg : TheFunction->args()) {
    AllocaInst *Alloca = CreateEntryBlockAlloca(TheFunction, arg.getName());

    Builder.CreateStore(&arg, Alloca);

    NamedValues[std::string(arg.getName())] = Alloca;
  }

  if (Value *retVal = Body->codegen()) {
    Builder.CreateRet(retVal);
    llvm::verifyFunction(*TheFunction);
    return TheFunction;
  }

  TheFunction->eraseFromParent();
  if (P.isBinaryOp())
    BinOpPrecedence.erase(Proto->getOperatorName());
  return nullptr;
}

cl::opt<std::string> HostName("hostname",
                              cl::desc("TCP hostname to connect to"),
                              cl::init("localhost"));

cl::opt<uint32_t> Port("port",
                        cl::desc("TCP port to connect to"),
                        cl::init(20000));

enum Token_type {
  EOF_TOKEN = 0,

  // primary
  NUMERIC_TOKEN,
  IDENTIFIER_TOKEN,

  // commands
  DEF_TOKEN,
  EXTERN_TOKEN,
  // control
  IF_TOKEN,
  THEN_TOKEN,
  ELSE_TOKEN,
  FOR_TOKEN,
  IN_TOKEN,

  // operators
  BINARY_TOKEN,
  UNARY_TOKEN,

  // var definition
  VAR_TOKEN,
};

static std::map<char, int> operator_precedence = {
    {'-', 1}, {'+', 2}, {'/', 3}, {'*', 4}};

static int getToken() {
  static char last_char = ' ';
  while (isspace(last_char)) last_char = getchar();

  if (isalpha(last_char)) {  // identifier: [a-zA-Z][a-zA-Z0-9]*
    identifier = last_char;
    while (isalnum(last_char = getchar())) identifier += last_char;

    if (identifier == "def") return DEF_TOKEN;
    if (identifier == "extern") return EXTERN_TOKEN;
    if (identifier == "if") return IF_TOKEN;
    if (identifier == "then") return THEN_TOKEN;
    if (identifier == "else") return ELSE_TOKEN;
    if (identifier == "for") return FOR_TOKEN;
    if (identifier == "in") return IN_TOKEN;
    if (identifier == "binary") return BINARY_TOKEN;
    if (identifier == "unary") return UNARY_TOKEN;
    if (identifier == "var") return VAR_TOKEN;
    return IDENTIFIER_TOKEN;
  }

  if (isdigit(last_char) || last_char == '.') {
    std::string num_str;
    do {
      num_str += last_char;
      last_char = getchar();
    } while (isdigit(last_char) || last_char == '.');

    numeric_val = stod(num_str, 0);
    return NUMERIC_TOKEN;
  }

  if (last_char == '#') {
    do {
      last_char = getchar();
    } while (last_char != EOF && last_char != '\n' && last_char != '\r');

    if (last_char != EOF) return getToken();
  }

  if (last_char == EOF) return EOF_TOKEN;

  char this_char = last_char;
  last_char = getchar();
  return this_char;
}

static int getNextToken() { return current_token = getToken(); }

static int getBinOpPrecedence() {
  if (!isascii(current_token)) return -1;

  int tok_prec = operator_precedence[current_token];
  if (tok_prec <= 0) return -1;
  return tok_prec;
}

static BaseASTPtr BaseParser();
static BaseASTPtr IdentifierParser();

static BaseASTPtr BinaryOpParser(int old_prec, BaseASTPtr LHS) {
  while (1) {
    int operator_prec = getBinOpPrecedence();
    if (operator_prec < old_prec) return LHS;

    int bin_op = current_token;
    getNextToken();

    auto RHS = BaseParser();
    if (!RHS) return nullptr;

    int next_prec = getBinOpPrecedence();
    if (operator_prec < next_prec) {
      RHS = BinaryOpParser(operator_prec + 1, std::move(RHS));
      if (!RHS) return nullptr;
    }

    LHS =
        std::make_unique<BinaryExprAST>(bin_op, std::move(LHS), std::move(RHS));
  }
}

static BaseASTPtr ExpressionParser() {
  auto LHS = BaseParser();
  if (!LHS) return nullptr;

  return BinaryOpParser(0, std::move(LHS));
}

/* parenexpr ::= '(' expression ')' */
static BaseASTPtr ParenParser() {
  getNextToken();
  auto V = ExpressionParser();
  if (!V) return nullptr;

  if (current_token != ')') return LogError("expected ')'");
  getNextToken();
  return V;
}

/* numberexpr ::= number */
static BaseASTPtr NumericParser() {
  auto result = std::make_unique<NumberExprAST>(numeric_val);
  getNextToken();
  return std::move(result);
}

/* identifierexpr
 *    ::= identifier
 *    ::= identifier '(' expression* ')'
 */
static BaseASTPtr IdentifierParser() {
  std::string id_name = identifier;

  getNextToken();

  if (current_token != '(') return std::make_unique<VariableExprAST>(id_name);

  getNextToken();

  std::vector<BaseASTPtr> args;

  if (current_token != ')') {
    while (true) {
      auto arg = ExpressionParser();
      if (!arg) return nullptr;
      args.push_back(std::move(arg));

      if (current_token == ')') break;

      if (current_token != ',')
        return LogError("Expect ')' or ',' in argument list");
      getNextToken();
    }
  }
  getNextToken();

  return std::make_unique<CallExprAST>(id_name, std::move(args));
}

static BaseASTPtr ParseIfExpr() {
  getNextToken();

  auto Cond = ExpressionParser();
  if (!Cond)
    return nullptr;

  if (current_token != THEN_TOKEN)
    return LogError("expected then");
  getNextToken();

  auto Then = ExpressionParser();
  if (!Then)
    return nullptr;

  if (current_token != ELSE_TOKEN)
    return LogError("Expected else");

  getNextToken();

  auto Else = ExpressionParser();
  if (!Else)
    return nullptr;

  return std::make_unique<IfExprAST>(std::move(Cond), std::move(Then),
                                     std::move(Else));
}

static BaseASTPtr ParseForExpr() {
  getNextToken();

  if (current_token != IDENTIFIER_TOKEN)
    return LogError("Expected identifier after for");

  std::string IdName = identifier;
  getNextToken();

  if (current_token != '=')
    return LogError("Expected '-' after for");
  getNextToken();

  auto Start = ExpressionParser();
  if (!Start)
    return nullptr;
  if (current_token != ',')
    return LogError("Expected ',' after for start value");
  getNextToken();

  auto End = ExpressionParser();
  if (!End)
    return nullptr;

  BaseASTPtr Step;
  if (current_token == ',') {
    getNextToken();
    Step = ExpressionParser();
    if (!Step)
      return nullptr;
  }

  if (current_token != IN_TOKEN)
    return LogError("Expected 'in' after for");
  getNextToken();

  auto Body = ExpressionParser();
  if (!Body)
    return nullptr;

  return std::make_unique<ForExprAST>(IdName, std::move(Start), std::move(End),
                                      std::move(Step), std::move(Body));
}

static BaseASTPtr ParseVarExpr() {
  getNextToken();

  std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;

  if (current_token != IDENTIFIER_TOKEN)
    return LogError("Expected identifier after var");

  while (true) {
    std::string Name = identifier;
    getNextToken();

    std::unique_ptr<ExprAST> Init = nullptr;
    if (current_token == '=') {
      getNextToken();

      Init = ExpressionParser();
      if (!Init)
        return nullptr;
    }

    VarNames.push_back(std::make_pair(Name, std::move(Init)));

    if (current_token != ',')
      break;
    getNextToken();

    if (current_token != IDENTIFIER_TOKEN)
      return LogError("Expected identifier list after var");
  }

  if (current_token != IN_TOKEN)
    return LogError("expected 'in' keyword after 'var'");

  auto Body = ExpressionParser();
  if (!Body)
    return nullptr;

  return std::make_unique<VarExprAST>(std::move(VarNames), std::move(Body));
}

static std::unique_ptr<ExprAST> BaseParser() {
  switch (current_token) {
  default:
    return LogError("unknown token when expecting an expression");
  case IDENTIFIER_TOKEN:
    return IdentifierParser();
  case NUMERIC_TOKEN:
    return NumericParser();
  case '(':
    return ParenParser();
  case IF_TOKEN:
    return ParseIfExpr();
  case FOR_TOKEN:
    return ParseForExpr();
  case VAR_TOKEN:
    return ParseVarExpr();
  }
}

static std::unique_ptr<ExprAST> ParseUnary() {
  if (!isascii(current_token) || current_token == '(' || current_token == ',')
    return BaseParser();

  int Opc = current_token;
  getNextToken();
  if (auto Operand = ParseUnary())
    return std::make_unique<UnaryExprAST>(Opc, std::move(Operand));
  return nullptr;
}

/* prototype
 *    ::= id '(' id* ')'
 */
static std::unique_ptr<PrototypeAST> ParsePrototype() {
  std::string FnName;

  unsigned Kind = 0;
  unsigned BinaryPrecedence = 30;

  switch(current_token) {
  default:
    return LogErrorP("Expected function name in prototype");
  case IDENTIFIER_TOKEN:
    FnName = identifier;
    Kind = 0;
    getNextToken();
    break;
  case UNARY_TOKEN:
    getNextToken();
    if (!isascii(current_token))
      return LogErrorP("Expected unary operator");
    FnName = "unary";
    FnName += (char)current_token;
    Kind = 1;
    getNextToken();
    break;
  case BINARY_TOKEN:
    getNextToken();
    if (!isascii(current_token))
      return LogErrorP("Expected binary operator");
    FnName = "binary";
    FnName += (char)current_token;
    Kind = 2;
    getNextToken();

    if (current_token == NUMERIC_TOKEN) {
      if (numeric_val < 1 || numeric_val > 100)
        return LogErrorP("Invalid precedence: must be 1..100");
      BinaryPrecedence = (unsigned)numeric_val;
      getNextToken();
    }
    break;
  }

  if (current_token != '(') return LogErrorP("Expected '(' in prototype");

  std::vector<std::string> func_arg_names;
  while (getNextToken() == IDENTIFIER_TOKEN)
    func_arg_names.push_back(identifier);

  if (current_token != ')') return LogErrorP("Expected ')' in prototype");

  getNextToken();

  if (Kind && func_arg_names.size() != Kind)
    return LogErrorP("Invalid number of operands for operator");

  return std::make_unique<PrototypeAST>(FnName, func_arg_names, Kind != 0,
                                        BinaryPrecedence);
}

/* definition ::= 'def' prototype expression */
static std::unique_ptr<FunctionAST> FunctionDefinitionParser() {
  getNextToken();
  auto def = ParsePrototype();
  if (!def) return nullptr;

  if (auto E = ExpressionParser())
    return std::make_unique<FunctionAST>(std::move(def), std::move(E));
  return nullptr;
}

static std::unique_ptr<PrototypeAST> ExternParser() {
  getNextToken();
  return ParsePrototype();
}

void InitializeModule() {
  TheModule = std::make_unique<Module>("my yaoyao", TheContext);
  TheModule->setDataLayout(TheJIT->getTargetMachine().createDataLayout());
}

std::unique_ptr<llvm::Module>
irgenAndTakeOwnership(FunctionAST &FnAST, const std::string &Suffix) {
  if (auto *F = FnAST.codegen()) {
    F->setName(F->getName() + Suffix);
    auto M = std::move(TheModule);
    InitializeModule();
    return M;
  } else
    report_fatal_error("Couldn't compile lazily JIT'd function");
}

static std::unique_ptr<FunctionAST> HandleTopExpression() {
  if (auto E = ExpressionParser()) {
    auto def = std::make_unique<PrototypeAST>("__anonymous_f_",
                                              std::vector<std::string>());
    return std::make_unique<FunctionAST>(std::move(def), std::move(E));
  }
  return nullptr;
}

static void HandleDefinition() {
  if (auto funcAST = FunctionDefinitionParser()) {
    fprintf(stderr, "Parsed a function definition\n");
    FunctionProtos[funcAST->getProto().getName()] =
      std::make_unique<PrototypeAST>(funcAST->getProto());
    ExitOnErr(TheJIT->addFunctionAST(std::move(funcAST)));
  } else {
    getNextToken();
  }
}

static void HandleExtern() {
  if (auto protoAST = ExternParser()) {
    if (auto *protoIR = protoAST->codegen()) {
      fprintf(stderr, "Parsed an extern: ");
      protoIR->print(errs());
      fprintf(stderr, "\n");
      FunctionProtos[protoIR->getName()] = std::move(protoAST);
    }
  } else {
    getNextToken();
  }
}

static void HandleTopLevelExpression() {
  if (auto funcAST = HandleTopExpression()) {
    fprintf(stderr, "Parsed a top-level expr\n");
    FunctionProtos[funcAST->getName()] =
        std::make_unique<PrototypeAST>(funcAST->getProto());
    if (auto *funcIR = funcAST->codegen()) {

      auto H = TheJIT->addModule(std::move(TheModule));
      InitializeModule();

      auto ExprSymbol = TheJIT->findSymbol("__anonymous_f_");
      assert(ExprSymbol && "Function Not Found");

      double (*FP)() = (double (*)())(intptr_t)cantFail(ExprSymbol.getAddress());
      fprintf(stderr, "Evaluated to %f\n", FP());
      TheJIT->removeModule(H);
    }
  } else {
    getNextToken();
  }
}

static void Driver() {
  while (1) {
    fprintf(stderr, "yaoyao > ");
    switch (current_token) {
      case ';':
        getNextToken();
        break;
      case EOF_TOKEN:
        return;
      case DEF_TOKEN:
        HandleDefinition();
        break;
      case EXTERN_TOKEN:
        HandleExtern();
        break;
      default:
        HandleTopLevelExpression();
        break;
    }
  }
}

std::unique_ptr<FDRPCChannel> connect() {
  int sockfd = socket(PF_INET, SOCK_STREAM, 0);
  hostent *server = gethostbyname(HostName.c_str());

  if (!server) {
    errs() << "Could not find host " << HostName << "\n";
    exit(1);
  }

  sockaddr_in servAddr;
  memset(&servAddr, 0, sizeof(servAddr));
  servAddr.sin_family = PF_INET;
  char *src;
  memcpy(&src, &server->h_addr, sizeof(char *));
  memcpy(&servAddr.sin_addr.s_addr, src, server->h_length);
  servAddr.sin_port = htons(Port);
  if (connect(sockfd, reinterpret_cast<sockaddr*>(&servAddr),
              sizeof(servAddr)) < 0) {
    errs() << "Failure to connect.\n";
    exit(1);
  }

  return std::make_unique<FDRPCChannel>(sockfd, sockfd);
}

int main(int argc, char *argv[]) {
  cl::ParseCommandLineOptions(argc, argv, "Yaoyao JTI - client.\n");
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();

  ExitOnErr.setBanner("yaoyao > ");

  BinOpPrecedence['='] = 2;
  BinOpPrecedence['<'] = 10;
  BinOpPrecedence['+'] = 20;
  BinOpPrecedence['-'] = 20;
  BinOpPrecedence['*'] = 40;

  ExecutionSession ES;
  auto TCPChannel = connect();
  auto Remote = ExitOnErr(RemoteCli::Create(*TCPChannel, ES));
  TheJIT = std::make_unique<YaoyaoJIT>();

  FunctionProtos["printExprResult"] =
    std::make_unique<PrototypeAST>("printExprResult",
                                   std::vector<std::string>({"Val"}));

  fprintf(stderr, "yaoyao > ");
  getNextToken();
  InitializeModule();

  Driver();

  TheJIT = nullptr;

  ExitOnErr(Remote->terminateSession());
  return 0;
}
