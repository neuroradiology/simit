#ifndef SIMIT_BACKEND_FUNCTION_H
#define SIMIT_BACKEND_FUNCTION_H

#include <cstdlib>
#include <vector>
#include <map>

#include "printable.h"
#include "uncopyable.h"

namespace simit {
class Set;

namespace ir {
class Func;
class Environment;
class Expr;
class Type;
class Var;
}

namespace backend {

class Function : public interfaces::Printable, interfaces::Uncopyable {
protected:
    Function(const ir::Func &func);

public:
  typedef std::function<void()> FuncType;
  virtual ~Function();

  /// Bind the given set to the set with the given name.
  virtual void bind(const std::string& name, simit::Set* set) = 0;

  /// Bind the given data to the tensor with the given name.
  virtual void bind(const std::string& name, void* data) = 0;

  /// Bind the given data and indices to the sparse tensor with the given name.
  virtual void bind(const std::string& name, const int* rowPtr,
                    const int* colInd, void* data) = 0;

  /// Initialize the function.
  virtual FuncType init() = 0;

  /// Query whether the function requires intialization.
  virtual bool isInitialized() = 0;

  // TODO Should these really be an extension to the bind interface?
  //      Per-argument updates/copies.
  //      Don't always write in a new pointer (requires re-JIT), just alert to
  //      updates in pointed-to data.
  virtual void mapArgs() {}
  virtual void unmapArgs(bool updated=true) {}

  /// Write the function to the stream. The output depends on the backend,
  /// for example the LLVM backend will write LLVM IR.
  virtual void print(std::ostream &os) const = 0;

  /// Print the function as machine assembly code to the stream.
  virtual void printMachine(std::ostream &os) const = 0;


  bool hasArg(std::string arg) const;
  const std::vector<std::string>& getArgs() const;
  const ir::Type& getArgType(std::string arg) const;

  bool hasGlobal(std::string name) const;
  const std::vector<std::string>& getGlobals() const;
  const ir::Type& getGlobalType(std::string global) const;

  bool hasBindable(std::string bindable) const;

private:
  ir::Environment* environment;

  std::vector<std::string> arguments;
  std::map<std::string, ir::Type> argumentTypes;

  /// We store the Simit Function's literals to prevent their memory from being
  /// reclaimed if the IR is deleted, as compiled functions are allowed to
  /// access them at runtime.
  std::vector<simit::ir::Expr> literals;
};

}}
#endif
