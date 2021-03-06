/* -*- mode: c++; c-basic-offset: 2; -*- */

//===-- main.cpp ------------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/ExecutionState.h"
#include "klee/Expr.h"
#include "klee/Interpreter.h"
#include "klee/Statistics.h"
#include "klee/Config/Version.h"
#include "klee/Internal/ADT/KTest.h"
#include "klee/Internal/ADT/TreeStream.h"
#include "klee/Internal/Support/Debug.h"
#include "klee/Internal/Support/ModuleUtil.h"
#include "klee/Internal/System/Time.h"
#include "klee/Internal/Support/PrintVersion.h"
#include "klee/Internal/Support/ErrorHandling.h"
#include "klee/Internal/Analysis/Annotator.h"


#if LLVM_VERSION_CODE > LLVM_VERSION(3, 2)
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#else
#include "llvm/Constants.h"
#include "llvm/Module.h"
#include "llvm/Type.h"
#include "llvm/InstrTypes.h"
#include "llvm/Instruction.h"
#include "llvm/Instructions.h"
#include "llvm/LLVMContext.h"
#include "llvm/Support/FileSystem.h"
#endif
#include "llvm/Support/Errno.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#if LLVM_VERSION_CODE < LLVM_VERSION(3, 0)
#include "llvm/Target/TargetSelect.h"
#else
#include "llvm/Support/TargetSelect.h"
#endif
#include "llvm/Support/Signals.h"

#if LLVM_VERSION_CODE < LLVM_VERSION(3, 5)
#include "llvm/Support/system_error.h"
#endif

#include <dirent.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <cerrno>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <iostream>

#include <mpi.h>
#include <set>

using namespace llvm;
using namespace klee;

#define START_PREFIX_TASK 0
#define KILL 1
#define FINISH 2
#define OFFLOAD 3
#define OFFLOAD_RESP 4
#define BUG_FOUND 5
#define TIMEOUT 6
#define NORMAL_TASK 7
#define KILL_COMP 8
#define READY_TO_OFFLOAD 9
#define NOT_READY_TO_OFFLOAD 10

#define PREFIX_MODE 101
#define RANGE_MODE 102
#define NO_MODE 103

#define OFFLOADING_ENABLE false
#define ENABLE_DYN_OFF false

#define ENABLE_CLEANUP false

#define FLUSH false

#define MASTER_NODE 0

enum searchMode{
  DFS,
  BFS,
  RAND,
  COVNEW
};

namespace {
  cl::opt<std::string>
  InputFile(cl::desc("<input bytecode>"), cl::Positional, cl::init("-"));

  cl::opt<std::string> SkippedFunctions(
      "skip-functions",
      cl::desc("Comma-separated list of functions to skip. "
               "Optionally, a line number can be specified to choose a specific call site "
               "(e.g. <function1>[:line],<function2>[:line],..)"));

  cl::opt<std::string>
  InlinedFunctions("inline",
                   cl::desc("Comma-separated list of functions to be inlined (e.g. <function1>,<function2>,..)"),
                   cl::init(""));

  cl::opt<unsigned int>
  MaxErrorCount("max-error-count",
                cl::desc("max error count before exit"),
                cl::init(0));

  cl::opt<std::string>
  ErrorLocation("error-location",
                cl::desc("Comma-separated list of locations where a failure is expected (e.g. <file1>[:line],<file2>[:line],..)"));

  cl::opt<std::string>
  EntryPoint("entry-point",
             cl::desc("Consider the function with the given name as the entrypoint"),
             cl::init("main"));

  cl::opt<std::string>
  RunInDir("run-in", cl::desc("Change to the given directory prior to executing"));

  cl::opt<std::string>
  Environ("environ", cl::desc("Parse environ from given file (in \"env\" format)"));

  cl::list<std::string>
  InputArgv(cl::ConsumeAfter,
            cl::desc("<program arguments>..."));

  cl::opt<bool>
  NoOutput("no-output",
           cl::desc("Don't generate test files"));

  cl::opt<bool>
  WarnAllExternals("warn-all-externals",
                   cl::desc("Give initial warning for all externals."));

  cl::opt<bool>
  WriteCVCs("write-cvcs",
            cl::desc("Write .cvc files for each test case"));

  cl::opt<bool>
  WriteKQueries("write-kqueries",
            cl::desc("Write .kquery files for each test case"));

  cl::opt<bool>
  WriteSMT2s("write-smt2s",
            cl::desc("Write .smt2 (SMT-LIBv2) files for each test case"));

  cl::opt<bool>
  WriteCov("write-cov",
           cl::desc("Write coverage information for each test case"));

  cl::opt<bool>
  WriteTestInfo("write-test-info",
                cl::desc("Write additional test case information"));

  cl::opt<bool>
  WritePaths("write-paths",
                cl::desc("Write .path files for each test case"));

  cl::opt<bool>
  WriteSymPaths("write-sym-paths",
                cl::desc("Write .sym.path files for each test case"));

  cl::opt<bool>
  ExitOnError("exit-on-error",
              cl::desc("Exit if errors occur"));


  enum LibcType {
    NoLibc, KleeLibc, UcLibc
  };

  cl::opt<LibcType>
  Libc("libc",
       cl::desc("Choose libc version (none by default)."),
       cl::values(clEnumValN(NoLibc, "none", "Don't link in a libc"),
                  clEnumValN(KleeLibc, "klee", "Link in klee libc"),
		  clEnumValN(UcLibc, "uclibc", "Link in uclibc (adapted for klee)"),
		  clEnumValEnd),
       cl::init(NoLibc));


  cl::opt<bool>
  WithPOSIXRuntime("posix-runtime",
		cl::desc("Link with POSIX runtime.  Options that can be passed as arguments to the programs are: --sym-arg <max-len>  --sym-args <min-argvs> <max-argvs> <max-len> + file model options"),
		cl::init(false));

  cl::opt<bool>
  WithSymArgsRuntime("sym-arg-runtime",
		cl::desc("Options that can be passed as arguments to the programs are: --sym-arg <max-len>  --sym-args <min-argvs> <max-argvs> <max-len>"),
		cl::init(false));

  cl::opt<bool>
  OptimizeModule("optimize",
                 cl::desc("Optimize before execution"),
		 cl::init(false));

  cl::opt<bool>
  CheckDivZero("check-div-zero",
               cl::desc("Inject checks for division-by-zero"),
               cl::init(true));

  cl::opt<bool>
  CheckOvershift("check-overshift",
               cl::desc("Inject checks for overshift"),
               cl::init(true));

  cl::opt<std::string>
  OutputDir("output-dir",
            cl::desc("Directory to write results in (defaults to klee-out-N)"),
            cl::init(""));

  cl::opt<bool>
  ReplayKeepSymbolic("replay-keep-symbolic",
                     cl::desc("Replay the test cases only by asserting "
                              "the bytes, not necessarily making them concrete."));

  cl::list<std::string>
      ReplayKTestFile("replay-ktest-file",
                      cl::desc("Specify a ktest file to use for replay"),
                      cl::value_desc("ktest file"));

  cl::list<std::string>
      ReplayKTestDir("replay-ktest-dir",
                   cl::desc("Specify a directory to replay ktest files from"),
                   cl::value_desc("output directory"));

  cl::opt<std::string>
  ReplayPathFile("replay-path",
                 cl::desc("Specify a path file to replay"),
                 cl::value_desc("path file"));

  cl::opt<unsigned int>
  phase1Depth("phase1Depth",
                 cl::desc("Depth to limit the exploration upto"),
                 cl::init(0));

  cl::opt<unsigned int>
  phase2Depth("phase2Depth",
                 cl::desc("Depth to limit the exploration upto"),
                 cl::init(0));

  cl::opt<unsigned int>
  timeOut("timeOut",
            cl::desc("time out command"),
            cl::init(0));

  cl::opt<std::string>
  pathFile("pathFile",
                 cl::desc("path file name"),
                 cl::value_desc("path file"),
                 cl::init("pathFile"));

  cl::opt<std::string>
  searchPolicy("searchPolicy",
                 cl::desc("policy name"),
                 cl::value_desc("policy name"),
                 cl::init("DFS"));

  cl::opt<std::string>
  offloadPolicy("offloadPolicy",
                 cl::desc("policy name"),
                 cl::value_desc("policy name"),
                 cl::init("DEFAULT"));

  cl::list<std::string>
  SeedOutFile("seed-out");

  cl::list<std::string>
  SeedOutDir("seed-out-dir");

  cl::list<std::string>
  LinkLibraries("link-llvm-lib",
		cl::desc("Link the given libraries before execution"),
		cl::value_desc("library file"));

  cl::opt<unsigned>
  MakeConcreteSymbolic("make-concrete-symbolic",
                       cl::desc("Probabilistic rate at which to make concrete reads symbolic, "
				"i.e. approximately 1 in n concrete reads will be made symbolic (0=off, 1=all).  "
				"Used for testing."),
                       cl::init(0));

  cl::opt<unsigned>
  StopAfterNTests("stop-after-n-tests",
	     cl::desc("Stop execution after generating the given number of tests.  Extra tests corresponding to partially explored paths will also be dumped."),
	     cl::init(0));

  cl::opt<bool>
  Watchdog("watchdog",
           cl::desc("Use a watchdog process to enforce --max-time."),
           cl::init(0));

  cl::opt<bool>
  lb("lb",
    	cl::desc("load balance"),
    	cl::init(false));
}

extern cl::opt<double> MaxTime;


typedef std::pair<const llvm::Value *, uint64_t> AllocSite;
typedef std::pair<llvm::Function *, AllocSite> ModInfo;
typedef std::map<ModInfo, uint32_t> ModInfoToIdMap;

typedef std::pair<unsigned, uint64_t> PSEAllocSite;
typedef std::pair<std::string, PSEAllocSite> PSEModInfo;
typedef std::map<PSEModInfo, uint32_t> PSEModInfoToIdMap;

typedef std::pair<std::string, uint64_t> PSEAllocSiteG;
typedef std::pair<std::string, PSEAllocSiteG> PSEModInfoG;
typedef std::map<PSEModInfoG, uint32_t> PSEModInfoToIdMapG;

typedef std::map<std::string, std::set<unsigned>> PSEModSetMap;
typedef std::map<unsigned, std::pair<std::set<PSEModInfo>, std::set<PSEModInfoG>>> PSELoadToModInfoMap;

/***/

class KleeHandler : public InterpreterHandler {
private:
  Interpreter *m_interpreter;
  TreeStreamWriter *m_pathWriter, *m_symPathWriter;
  llvm::raw_ostream *m_infoFile;

  SmallString<128> m_outputDirectory;
  std::string outputFileName;

  unsigned m_testIndex;  // number of tests written so far
  unsigned m_pathsExplored; // number of paths explored so far
  unsigned m_recoveryStatesCount; // number of recovery states
  unsigned m_generatedSlicesCount; // number of generated slices
  unsigned m_snapshotsCount; // number of created snapshots

  // used for writing .ktest files
  int m_argc;
  char **m_argv;

public:
  KleeHandler(int argc, char **argv);
  ~KleeHandler();

  llvm::raw_ostream &getInfoStream() const { return *m_infoFile; }
  unsigned getNumTestCases() { return m_testIndex; }
  unsigned getNumPathsExplored() { return m_pathsExplored; }
  void incPathsExplored() { m_pathsExplored++; }

  unsigned getRecoveryStatesCount() {
    return m_recoveryStatesCount;
  }

  void incRecoveryStatesCount() {
    m_recoveryStatesCount++;
  }

  unsigned getGeneratedSlicesCount() {
    return m_generatedSlicesCount;
  }

  void incGeneratedSlicesCount() {
    m_generatedSlicesCount++;
  }

  unsigned getSnapshotsCount() {
    return m_snapshotsCount;
  }

  void incSnapshotsCount() {
    m_snapshotsCount++;
  }

  void setInterpreter(Interpreter *i);

  std::string getOutputDir();

  void processTestCase(const ExecutionState  &state,
                       const char *errorMessage,
                       const char *errorSuffix);

  std::string getOutputFilename(const std::string &filename);
  llvm::raw_fd_ostream *openOutputFile(const std::string &filename, bool openOutside = false);
  std::string getTestFilename(const std::string &suffix, unsigned id);
  llvm::raw_fd_ostream *openTestFile(const std::string &suffix, unsigned id);

  // load a .path file
  static void loadPathFile(std::string name,
                           std::vector<bool> &buffer);

  static void getKTestFilesInDir(std::string directoryPath,
                                 std::vector<std::string> &results);

  static std::string getRunTimeLibraryPath(const char *argv0);
};

KleeHandler::KleeHandler(int argc, char **argv)
  : m_interpreter(0),
    m_pathWriter(0),
    m_symPathWriter(0),
    m_infoFile(0),
    m_outputDirectory(),
    m_testIndex(0),
    m_pathsExplored(0),
    m_recoveryStatesCount(0),
    m_generatedSlicesCount(0),
    m_snapshotsCount(0),
    m_argc(argc),
    m_argv(argv) {

  // create output directory (OutputDir or "klee-out-<i>")
  bool dir_given = OutputDir != "";
  SmallString<128> directory(dir_given ? OutputDir : InputFile);

  if (!dir_given) sys::path::remove_filename(directory);
#if LLVM_VERSION_CODE < LLVM_VERSION(3, 5)
  error_code ec;
  if ((ec = sys::fs::make_absolute(directory)) != errc::success) {
#else
  if (auto ec = sys::fs::make_absolute(directory)) {
#endif
    klee_error("unable to determine absolute path: %s", ec.message().c_str());
  }

	if(!dir_given) {
		klee_error("Output Directory not Provided");
  } else {
    // "klee-out-<i>"
    int i = 0;
    for (; i <= INT_MAX; ++i) {
      SmallString<128> d(directory);
      //llvm::sys::path::append(d, "klee-out-");
      raw_svector_ostream ds(d); ds << i; ds.flush();

      // create directory and try to link klee-last
      if (mkdir(d.c_str(), 0775) == 0) {
        m_outputDirectory = d;
        outputFileName = OutputDir+std::to_string(i);
        int world_rank;
        MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
        std::cout<<"Output Directory World Rank: "<<world_rank<<" Index: "
                 <<outputFileName<<"\n";

        SmallString<128> klee_last(directory);
        llvm::sys::path::append(klee_last, "klee-last");

        if (((unlink(klee_last.c_str()) < 0) && (errno != ENOENT)) ||
            symlink(m_outputDirectory.c_str(), klee_last.c_str()) < 0) {

          klee_warning("cannot create klee-last symlink: %s", strerror(errno));
        }
        break;
      }
      // otherwise try again or exit on error
      if (errno != EEXIST)
        klee_error("cannot create \"%s\": %s", m_outputDirectory.c_str(), strerror(errno));
    }
    if (i == INT_MAX && m_outputDirectory.str().equals(""))
        klee_error("cannot create output directory: index out of range");
  }

  klee_message("output directory is \"%s\"", m_outputDirectory.c_str());

  // open warnings.txt
  std::string file_path = getOutputFilename("warnings.txt");
  if ((klee_warning_file = fopen(file_path.c_str(), "w")) == NULL)
    klee_error("cannot open file \"%s\": %s", file_path.c_str(), strerror(errno));

  // open messages.txt
  file_path = getOutputFilename("messages.txt");
  if ((klee_message_file = fopen(file_path.c_str(), "w")) == NULL)
    klee_error("cannot open file \"%s\": %s", file_path.c_str(), strerror(errno));

  // open info
  m_infoFile = openOutputFile("info");
}

std::string KleeHandler::getOutputDir() {
    return outputFileName;
}

KleeHandler::~KleeHandler() {
  if (m_pathWriter) delete m_pathWriter;
  if (m_symPathWriter) delete m_symPathWriter;
  fclose(klee_warning_file);
  fclose(klee_message_file);
  delete m_infoFile;
}

void KleeHandler::setInterpreter(Interpreter *i) {
  m_interpreter = i;

  if (false) {
    m_pathWriter = new TreeStreamWriter(getOutputFilename("paths.ts"));
    assert(m_pathWriter->good());
    m_interpreter->setPathWriter(m_pathWriter);
  }

  if (false) {
    m_symPathWriter = new TreeStreamWriter(getOutputFilename("symPaths.ts"));
    assert(m_symPathWriter->good());
    m_interpreter->setSymbolicPathWriter(m_symPathWriter);
  }
}

std::string KleeHandler::getOutputFilename(const std::string &filename) {
  SmallString<128> path = m_outputDirectory;
  sys::path::append(path,filename);
  return path.str();
}

llvm::raw_fd_ostream *KleeHandler::openOutputFile(const std::string &filename, bool openOutside) {
  std::string path;
  llvm::raw_fd_ostream *f;
  std::string Error;
  if(openOutside) {
    path = filename;
  } else {
    path = getOutputFilename(filename);
  }
#if LLVM_VERSION_CODE >= LLVM_VERSION(3,5)
  f = new llvm::raw_fd_ostream(path.c_str(), Error, llvm::sys::fs::F_None);
#elif LLVM_VERSION_CODE >= LLVM_VERSION(3,4)
  f = new llvm::raw_fd_ostream(path.c_str(), Error, llvm::sys::fs::F_Binary);
#else
  f = new llvm::raw_fd_ostream(path.c_str(), Error, llvm::raw_fd_ostream::F_Binary);
#endif
  if (!Error.empty()) {
    klee_warning("error opening file \"%s\".  KLEE may have run out of file "
               "descriptors: try to increase the maximum number of open file "
               "descriptors by using ulimit (%s).",
               filename.c_str(), Error.c_str());
    delete f;
    f = NULL;
  }

  return f;
}

std::string KleeHandler::getTestFilename(const std::string &suffix, unsigned id) {
  std::stringstream filename;
  filename << "test" << std::setfill('0') << std::setw(6) << id << '.' << suffix;
  return filename.str();
}

llvm::raw_fd_ostream *KleeHandler::openTestFile(const std::string &suffix,
                                                unsigned id) {
  return openOutputFile(getTestFilename(suffix, id));
}


/* Outputs all files (.ktest, .kquery, .cov etc.) describing a test case */
void KleeHandler::processTestCase(const ExecutionState &state,
                                  const char *errorMessage,
                                  const char *errorSuffix) {
  if (errorMessage && ExitOnError) {
    llvm::errs() << "EXITING ON ERROR:\n" << errorMessage << "\n";
    exit(1);
  }

  if (!NoOutput) {
    std::vector< std::pair<std::string, std::vector<unsigned char> > > out;
    bool success = m_interpreter->getSymbolicSolution(state, out);

    if (!success)
      klee_warning("unable to get symbolic solution, losing test case");

    double start_time = util::getWallTime();

    unsigned id = ++m_testIndex;

    if (success) {
      KTest b;
      b.numArgs = m_argc;
      b.args = m_argv;
      b.symArgvs = 0;
      b.symArgvLen = 0;
      b.numObjects = out.size();
      b.objects = new KTestObject[b.numObjects];
      assert(b.objects);
      for (unsigned i=0; i<b.numObjects; i++) {
        KTestObject *o = &b.objects[i];
        o->name = const_cast<char*>(out[i].first.c_str());
        o->numBytes = out[i].second.size();
        o->bytes = new unsigned char[o->numBytes];
        assert(o->bytes);
        std::copy(out[i].second.begin(), out[i].second.end(), o->bytes);
      }

      if (!kTest_toFile(&b, getOutputFilename(getTestFilename("ktest", id)).c_str())) {
        klee_warning("unable to write output test case, losing it");
      }

      for (unsigned i=0; i<b.numObjects; i++)
        delete[] b.objects[i].bytes;
      delete[] b.objects;
    }

    if (errorMessage) {
      llvm::raw_ostream *f = openTestFile(errorSuffix, id);
      *f << errorMessage;
      delete f;
    }

    if (m_pathWriter) {
      std::vector<unsigned char> concreteBranches;
      m_pathWriter->readStream(m_interpreter->getPathStreamID(state),
                               concreteBranches);
      llvm::raw_fd_ostream *f = openTestFile("path", id);
      for (std::vector<unsigned char>::iterator I = concreteBranches.begin(),
                                                E = concreteBranches.end();
           I != E; ++I) {
        *f << *I << "\n";
      }
      delete f;
    }

    if (errorMessage || WriteKQueries) {
      std::string constraints;
      m_interpreter->getConstraintLog(state, constraints,Interpreter::KQUERY);
      llvm::raw_ostream *f = openTestFile("kquery", id);
      *f << constraints;
      delete f;
    }

    if (WriteCVCs) {
      // FIXME: If using Z3 as the core solver the emitted file is actually
      // SMT-LIBv2 not CVC which is a bit confusing
      std::string constraints;
      m_interpreter->getConstraintLog(state, constraints, Interpreter::STP);
      llvm::raw_ostream *f = openTestFile("cvc", id);
      *f << constraints;
      delete f;
    }

    if(WriteSMT2s) {
      std::string constraints;
        m_interpreter->getConstraintLog(state, constraints, Interpreter::SMTLIB2);
        llvm::raw_ostream *f = openTestFile("smt2", id);
        *f << constraints;
        delete f;
    }

    if (m_symPathWriter) {
      std::vector<unsigned char> symbolicBranches;
      m_symPathWriter->readStream(m_interpreter->getSymbolicPathStreamID(state),
                                  symbolicBranches);
      llvm::raw_fd_ostream *f = openTestFile("sym.path", id);
      for (std::vector<unsigned char>::iterator I = symbolicBranches.begin(), E = symbolicBranches.end(); I!=E; ++I) {
        *f << *I << "\n";
      }
      delete f;
    }

    if (WriteCov) {
      std::map<const std::string*, std::set<unsigned> > cov;
      m_interpreter->getCoveredLines(state, cov);
      llvm::raw_ostream *f = openTestFile("cov", id);
      for (std::map<const std::string*, std::set<unsigned> >::iterator
             it = cov.begin(), ie = cov.end();
           it != ie; ++it) {
        for (std::set<unsigned>::iterator
               it2 = it->second.begin(), ie = it->second.end();
             it2 != ie; ++it2)
          *f << *it->first << ":" << *it2 << "\n";
      }
      delete f;
    }

    if (m_testIndex == StopAfterNTests)
      m_interpreter->setHaltExecution(true);

    if (WriteTestInfo) {
      double elapsed_time = util::getWallTime() - start_time;
      llvm::raw_ostream *f = openTestFile("info", id);
      *f << "Time to generate test case: "
         << elapsed_time << "s\n";
      delete f;
    }
  }
}

  // load a .path file
void KleeHandler::loadPathFile(std::string name,
                                     std::vector<bool> &buffer) {
  std::ifstream f(name.c_str(), std::ios::in | std::ios::binary);

  if (!f.good())
    assert(0 && "unable to open path file");

  while (f.good()) {
    unsigned value;
    f >> value;
    buffer.push_back(!!value);
    f.get();
  }
}

void KleeHandler::getKTestFilesInDir(std::string directoryPath,
                                     std::vector<std::string> &results) {
#if LLVM_VERSION_CODE < LLVM_VERSION(3, 5)
  error_code ec;
#else
  std::error_code ec;
#endif
  for (llvm::sys::fs::directory_iterator i(directoryPath, ec), e; i != e && !ec;
       i.increment(ec)) {
    std::string f = (*i).path();
    if (f.substr(f.size()-6,f.size()) == ".ktest") {
          results.push_back(f);
    }
  }

  if (ec) {
    llvm::errs() << "ERROR: unable to read output directory: " << directoryPath
                 << ": " << ec.message() << "\n";
    exit(1);
  }
}

std::string KleeHandler::getRunTimeLibraryPath(const char *argv0) {
  // allow specifying the path to the runtime library
  const char *env = getenv("KLEE_RUNTIME_LIBRARY_PATH");
  if (env)
    return std::string(env);

  // Take any function from the execution binary but not main (as not allowed by
  // C++ standard)
  void *MainExecAddr = (void *)(intptr_t)getRunTimeLibraryPath;
  SmallString<128> toolRoot(
      #if LLVM_VERSION_CODE >= LLVM_VERSION(3,4)
      llvm::sys::fs::getMainExecutable(argv0, MainExecAddr)
      #else
      llvm::sys::Path::GetMainExecutable(argv0, MainExecAddr).str()
      #endif
      );

  // Strip off executable so we have a directory path
  llvm::sys::path::remove_filename(toolRoot);

  SmallString<128> libDir;

  if (strlen( KLEE_INSTALL_BIN_DIR ) != 0 &&
      strlen( KLEE_INSTALL_RUNTIME_DIR ) != 0 &&
      toolRoot.str().endswith( KLEE_INSTALL_BIN_DIR ))
  {
    KLEE_DEBUG_WITH_TYPE("klee_runtime", llvm::dbgs() <<
                         "Using installed KLEE library runtime: ");
    libDir = toolRoot.str().substr(0, 
               toolRoot.str().size() - strlen( KLEE_INSTALL_BIN_DIR ));
    llvm::sys::path::append(libDir, KLEE_INSTALL_RUNTIME_DIR);
  }
  else
  {
    KLEE_DEBUG_WITH_TYPE("klee_runtime", llvm::dbgs() <<
                         "Using build directory KLEE library runtime :");
    libDir = KLEE_DIR;
    llvm::sys::path::append(libDir,RUNTIME_CONFIGURATION);
    llvm::sys::path::append(libDir,"lib");
  }

  KLEE_DEBUG_WITH_TYPE("klee_runtime", llvm::dbgs() <<
                       libDir.c_str() << "\n");
  return libDir.str();
}

//===----------------------------------------------------------------------===//
// main Driver function
//
static std::string strip(std::string &in) {
  unsigned len = in.size();
  unsigned lead = 0, trail = len;
  while (lead<len && isspace(in[lead]))
    ++lead;
  while (trail>lead && isspace(in[trail-1]))
    --trail;
  return in.substr(lead, trail-lead);
}

static void parseArguments(int argc, char **argv) {
  cl::SetVersionPrinter(klee::printVersion);
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 2)
  // This version always reads response files
  cl::ParseCommandLineOptions(argc, argv, " klee\n");
#else
  cl::ParseCommandLineOptions(argc, argv, " klee\n", /*ReadResponseFiles=*/ true);
#endif
}

static int initEnv(Module *mainModule) {

  /*
    nArgcP = alloc oldArgc->getType()
    nArgvV = alloc oldArgv->getType()
    store oldArgc nArgcP
    store oldArgv nArgvP
    klee_init_environment(nArgcP, nArgvP)
    nArgc = load nArgcP
    nArgv = load nArgvP
    oldArgc->replaceAllUsesWith(nArgc)
    oldArgv->replaceAllUsesWith(nArgv)
  */

  Function *mainFn = mainModule->getFunction(EntryPoint);
  if (!mainFn) {
    klee_error("'%s' function not found in module.", EntryPoint.c_str());
  }

  if (mainFn->arg_size() < 2) {
    klee_error("Cannot handle ""--posix-runtime"" when main() has less than two arguments.\n");
  }

  Instruction* firstInst = mainFn->begin()->begin();

  Value* oldArgc = mainFn->arg_begin();
  Value* oldArgv = ++mainFn->arg_begin();

  AllocaInst* argcPtr =
    new AllocaInst(oldArgc->getType(), "argcPtr", firstInst);
  AllocaInst* argvPtr =
    new AllocaInst(oldArgv->getType(), "argvPtr", firstInst);

  /* Insert void klee_init_env(int* argc, char*** argv) */
  std::vector<const Type*> params;
  params.push_back(Type::getInt32Ty(getGlobalContext()));
  params.push_back(Type::getInt32Ty(getGlobalContext()));

  Function* initEnvFn = NULL;
  if (WithPOSIXRuntime)
  initEnvFn =
    cast<Function>(mainModule->getOrInsertFunction("klee_init_env",
                                                   Type::getVoidTy(getGlobalContext()),
                                                   argcPtr->getType(),
                                                   argvPtr->getType(),
                                                   NULL));
  if (WithSymArgsRuntime)
	  initEnvFn =
	    cast<Function>(mainModule->getOrInsertFunction("klee_init_args",
	                                                   Type::getVoidTy(getGlobalContext()),
	                                                   argcPtr->getType(),
	                                                   argvPtr->getType(),
	                                                   NULL));
  assert(initEnvFn);
  std::vector<Value*> args;
  args.push_back(argcPtr);
  args.push_back(argvPtr);
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 0)
  Instruction* initEnvCall = CallInst::Create(initEnvFn, args,
					      "", firstInst);
#else
  Instruction* initEnvCall = CallInst::Create(initEnvFn, args.begin(), args.end(),
					      "", firstInst);
#endif
  Value *argc = new LoadInst(argcPtr, "newArgc", firstInst);
  Value *argv = new LoadInst(argvPtr, "newArgv", firstInst);

  oldArgc->replaceAllUsesWith(argc);
  oldArgv->replaceAllUsesWith(argv);

  new StoreInst(oldArgc, argcPtr, initEnvCall);
  new StoreInst(oldArgv, argvPtr, initEnvCall);

  return 0;
}


// This is a terrible hack until we get some real modeling of the
// system. All we do is check the undefined symbols and warn about
// any "unrecognized" externals and about any obviously unsafe ones.

// Symbols we explicitly support
static const char *modelledExternals[] = {
  "_ZTVN10__cxxabiv117__class_type_infoE",
  "_ZTVN10__cxxabiv120__si_class_type_infoE",
  "_ZTVN10__cxxabiv121__vmi_class_type_infoE",

  // special functions
  "_assert",
  "__assert_fail",
  "__assert_rtn",
  "calloc",
  "_exit",
  "exit",
  "free",
  "abort",
  "klee_abort",
  "klee_assume",
  "klee_check_memory_access",
  "klee_define_fixed_object",
  "klee_get_errno",
  "klee_get_valuef",
  "klee_get_valued",
  "klee_get_valuel",
  "klee_get_valuell",
  "klee_get_value_i32",
  "klee_get_value_i64",
  "klee_get_obj_size",
  "klee_is_symbolic",
  "klee_make_symbolic",
  "klee_mark_global",
  "klee_merge",
  "klee_prefer_cex",
  "klee_posix_prefer_cex",
  "klee_print_expr",
  "klee_print_range",
  "klee_report_error",
  "klee_set_forking",
  "klee_silent_exit",
  "klee_warning",
  "klee_warning_once",
  "klee_alias_function",
  "klee_stack_trace",
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 1)
  "llvm.dbg.declare",
  "llvm.dbg.value",
#endif
  "llvm.va_start",
  "llvm.va_end",
  "malloc",
  "realloc",
  "_ZdaPv",
  "_ZdlPv",
  "_Znaj",
  "_Znwj",
  "_Znam",
  "_Znwm",
  "__ubsan_handle_add_overflow",
  "__ubsan_handle_sub_overflow",
  "__ubsan_handle_mul_overflow",
  "__ubsan_handle_divrem_overflow",
};
// Symbols we aren't going to warn about
static const char *dontCareExternals[] = {
#if 0
  // stdio
  "fprintf",
  "fflush",
  "fopen",
  "fclose",
  "fputs_unlocked",
  "putchar_unlocked",
  "vfprintf",
  "fwrite",
  "puts",
  "printf",
  "stdin",
  "stdout",
  "stderr",
  "_stdio_term",
  "__errno_location",
  "fstat",
#endif

  // static information, pretty ok to return
  "getegid",
  "geteuid",
  "getgid",
  "getuid",
  "getpid",
  "gethostname",
  "getpgrp",
  "getppid",
  "getpagesize",
  "getpriority",
  "getgroups",
  "getdtablesize",
  "getrlimit",
  "getrlimit64",
  "getcwd",
  "getwd",
  "gettimeofday",
  "uname",

  // fp stuff we just don't worry about yet
  "frexp",
  "ldexp",
  "__isnan",
  "__signbit",
};
// Extra symbols we aren't going to warn about with klee-libc
static const char *dontCareKlee[] = {
  "__ctype_b_loc",
  "__ctype_get_mb_cur_max",

  // io system calls
  "open",
  "write",
  "read",
  "close",
};
// Extra symbols we aren't going to warn about with uclibc
static const char *dontCareUclibc[] = {
  "__dso_handle",

  // Don't warn about these since we explicitly commented them out of
  // uclibc.
  "printf",
  "vprintf"
};
// Symbols we consider unsafe
static const char *unsafeExternals[] = {
  "fork", // oh lord
  "exec", // heaven help us
  "error", // calls _exit
  "raise", // yeah
  "kill", // mmmhmmm
};
#define NELEMS(array) (sizeof(array)/sizeof(array[0]))
void externalsAndGlobalsCheck(const Module *m) {
  std::map<std::string, bool> externals;
  std::set<std::string> modelled(modelledExternals,
                                 modelledExternals+NELEMS(modelledExternals));
  std::set<std::string> dontCare(dontCareExternals,
                                 dontCareExternals+NELEMS(dontCareExternals));
  std::set<std::string> unsafe(unsafeExternals,
                               unsafeExternals+NELEMS(unsafeExternals));

  switch (Libc) {
  case KleeLibc:
    dontCare.insert(dontCareKlee, dontCareKlee+NELEMS(dontCareKlee));
    break;
  case UcLibc:
    dontCare.insert(dontCareUclibc,
                    dontCareUclibc+NELEMS(dontCareUclibc));
    break;
  case NoLibc: /* silence compiler warning */
    break;
  }

  if (WithPOSIXRuntime)
    dontCare.insert("syscall");

  for (Module::const_iterator fnIt = m->begin(), fn_ie = m->end();
       fnIt != fn_ie; ++fnIt) {
    if (fnIt->isDeclaration() && !fnIt->use_empty())
      externals.insert(std::make_pair(fnIt->getName(), false));
    for (Function::const_iterator bbIt = fnIt->begin(), bb_ie = fnIt->end();
         bbIt != bb_ie; ++bbIt) {
      for (BasicBlock::const_iterator it = bbIt->begin(), ie = bbIt->end();
           it != ie; ++it) {
        if (const CallInst *ci = dyn_cast<CallInst>(it)) {
          if (isa<InlineAsm>(ci->getCalledValue())) {
            klee_warning_once(&*fnIt,
                              "function \"%s\" has inline asm",
                              fnIt->getName().data());
          }
        }
      }
    }
  }
  for (Module::const_global_iterator
         it = m->global_begin(), ie = m->global_end();
       it != ie; ++it)
    if (it->isDeclaration() && !it->use_empty())
      externals.insert(std::make_pair(it->getName(), true));
  // and remove aliases (they define the symbol after global
  // initialization)
  for (Module::const_alias_iterator
         it = m->alias_begin(), ie = m->alias_end();
       it != ie; ++it) {
    std::map<std::string, bool>::iterator it2 =
      externals.find(it->getName());
    if (it2!=externals.end())
      externals.erase(it2);
  }

  std::map<std::string, bool> foundUnsafe;
  for (std::map<std::string, bool>::iterator
         it = externals.begin(), ie = externals.end();
       it != ie; ++it) {
    const std::string &ext = it->first;
    if (!modelled.count(ext) && (WarnAllExternals ||
                                 !dontCare.count(ext))) {
      if (unsafe.count(ext)) {
        foundUnsafe.insert(*it);
      } else {
        klee_warning("undefined reference to %s: %s",
                     it->second ? "variable" : "function",
                     ext.c_str());
      }
    }
  }

  for (std::map<std::string, bool>::iterator
         it = foundUnsafe.begin(), ie = foundUnsafe.end();
       it != ie; ++it) {
    const std::string &ext = it->first;
    klee_warning("undefined reference to %s: %s (UNSAFE)!",
                 it->second ? "variable" : "function",
                 ext.c_str());
  }
}

static Interpreter *theInterpreter = 0;

static bool interrupted = false;

// Pulled out so it can be easily called from a debugger.
extern "C"
void halt_execution() {
  theInterpreter->setHaltExecution(true);
}

extern "C"
void stop_forking() {
  theInterpreter->setInhibitForking(true);
}

static void interrupt_handle() {
  if (!interrupted && theInterpreter) {
    llvm::errs() << "KLEE: ctrl-c detected, requesting interpreter to halt.\n";
    halt_execution();
    sys::SetInterruptFunction(interrupt_handle);
  } else {
    llvm::errs() << "KLEE: ctrl-c detected, exiting.\n";
    exit(1);
  }
  interrupted = true;
}

// returns the end of the string put in buf
static char *format_tdiff(char *buf, long seconds)
{
  assert(seconds >= 0);

  long minutes = seconds / 60;  seconds %= 60;
  long hours   = minutes / 60;  minutes %= 60;
  long days    = hours   / 24;  hours   %= 24;

  buf = strrchr(buf, '\0');
  if (days > 0) buf += sprintf(buf, "%ld days, ", days);
  buf += sprintf(buf, "%02ld:%02ld:%02ld", hours, minutes, seconds);
  return buf;
}

#ifndef SUPPORT_KLEE_UCLIBC
static llvm::Module *linkWithUclibc(llvm::Module *mainModule, StringRef libDir) {
  klee_error("invalid libc, no uclibc support!\n");
}
#else
static void replaceOrRenameFunction(llvm::Module *module,
		const char *old_name, const char *new_name)
{
  Function *f, *f2;
  f = module->getFunction(new_name);
  f2 = module->getFunction(old_name);
  if (f2) {
    if (f) {
      f2->replaceAllUsesWith(f);
      f2->eraseFromParent();
    } else {
      f2->setName(new_name);
      assert(f2->getName() == new_name);
    }
  }
}
static llvm::Module *linkWithUclibc(llvm::Module *mainModule, StringRef libDir) {
  // Ensure that klee-uclibc exists
  SmallString<128> uclibcBCA(libDir);
  llvm::sys::path::append(uclibcBCA, KLEE_UCLIBC_BCA_NAME);

  bool uclibcExists=false;
  llvm::sys::fs::exists(uclibcBCA.c_str(), uclibcExists);
  if (!uclibcExists)
    klee_error("Cannot find klee-uclibc : %s", uclibcBCA.c_str());

  Function *f;
  // force import of __uClibc_main
  mainModule->getOrInsertFunction("__uClibc_main",
                                  FunctionType::get(Type::getVoidTy(getGlobalContext()),
                                  std::vector<LLVM_TYPE_Q Type*>(),
                                  true));

  // force various imports
  if (WithPOSIXRuntime) {
    LLVM_TYPE_Q llvm::Type *i8Ty = Type::getInt8Ty(getGlobalContext());
    mainModule->getOrInsertFunction("realpath",
                                    PointerType::getUnqual(i8Ty),
                                    PointerType::getUnqual(i8Ty),
                                    PointerType::getUnqual(i8Ty),
                                    NULL);
    mainModule->getOrInsertFunction("getutent",
                                    PointerType::getUnqual(i8Ty),
                                    NULL);
    mainModule->getOrInsertFunction("__fgetc_unlocked",
                                    Type::getInt32Ty(getGlobalContext()),
                                    PointerType::getUnqual(i8Ty),
                                    NULL);
    mainModule->getOrInsertFunction("__fputc_unlocked",
                                    Type::getInt32Ty(getGlobalContext()),
                                    Type::getInt32Ty(getGlobalContext()),
                                    PointerType::getUnqual(i8Ty),
                                    NULL);
  }

  f = mainModule->getFunction("__ctype_get_mb_cur_max");
  if (f) f->setName("_stdlib_mb_cur_max");

  // Strip of asm prefixes for 64 bit versions because they are not
  // present in uclibc and we want to make sure stuff will get
  // linked. In the off chance that both prefixed and unprefixed
  // versions are present in the module, make sure we don't create a
  // naming conflict.
  for (Module::iterator fi = mainModule->begin(), fe = mainModule->end();
       fi != fe; ++fi) {
    Function *f = fi;
    const std::string &name = f->getName();
    if (name[0]=='\01') {
      unsigned size = name.size();
      if (name[size-2]=='6' && name[size-1]=='4') {
        std::string unprefixed = name.substr(1);

        // See if the unprefixed version exists.
        if (Function *f2 = mainModule->getFunction(unprefixed)) {
          f->replaceAllUsesWith(f2);
          f->eraseFromParent();
        } else {
          f->setName(unprefixed);
        }
      }
    }
  }

  mainModule = klee::linkWithLibrary(mainModule, uclibcBCA.c_str());
  assert(mainModule && "unable to link with uclibc");


  replaceOrRenameFunction(mainModule, "__libc_open", "open");
  replaceOrRenameFunction(mainModule, "__libc_fcntl", "fcntl");

  // Take care of fortified functions
  replaceOrRenameFunction(mainModule, "__fprintf_chk", "fprintf");

  // XXX we need to rearchitect so this can also be used with
  // programs externally linked with uclibc.

  // We now need to swap things so that __uClibc_main is the entry
  // point, in such a way that the arguments are passed to
  // __uClibc_main correctly. We do this by renaming the user main
  // and generating a stub function to call __uClibc_main. There is
  // also an implicit cooperation in that runFunctionAsMain sets up
  // the environment arguments to what uclibc expects (following
  // argv), since it does not explicitly take an envp argument.
  Function *userMainFn = mainModule->getFunction(EntryPoint);
  assert(userMainFn && "unable to get user main");
  Function *uclibcMainFn = mainModule->getFunction("__uClibc_main");
  assert(uclibcMainFn && "unable to get uclibc main");
  userMainFn->setName("__user_main");

  const FunctionType *ft = uclibcMainFn->getFunctionType();
  assert(ft->getNumParams() == 7);

  std::vector<LLVM_TYPE_Q Type*> fArgs;
  fArgs.push_back(ft->getParamType(1)); // argc
  fArgs.push_back(ft->getParamType(2)); // argv
  Function *stub = Function::Create(FunctionType::get(Type::getInt32Ty(getGlobalContext()), fArgs, false),
                                    GlobalVariable::ExternalLinkage,
                                    EntryPoint,
                                    mainModule);
  BasicBlock *bb = BasicBlock::Create(getGlobalContext(), "entry", stub);

  std::vector<llvm::Value*> args;
  args.push_back(llvm::ConstantExpr::getBitCast(userMainFn,
                                                ft->getParamType(0)));
  args.push_back(stub->arg_begin()); // argc
  args.push_back(++stub->arg_begin()); // argv
  args.push_back(Constant::getNullValue(ft->getParamType(3))); // app_init
  args.push_back(Constant::getNullValue(ft->getParamType(4))); // app_fini
  args.push_back(Constant::getNullValue(ft->getParamType(5))); // rtld_fini
  args.push_back(Constant::getNullValue(ft->getParamType(6))); // stack_end
#if LLVM_VERSION_CODE >= LLVM_VERSION(3, 0)
  CallInst::Create(uclibcMainFn, args, "", bb);
#else
  CallInst::Create(uclibcMainFn, args.begin(), args.end(), "", bb);
#endif

  new UnreachableInst(getGlobalContext(), bb);

  klee_message("NOTE: Using klee-uclibc : %s", uclibcBCA.c_str());
  return mainModule;
}
#endif

bool parseNameLineOption(
    std::string option,
    std::string &fname,
    std::vector<unsigned int> &lines
) {
    std::istringstream stream(option);
    std::string token;
    char *endptr;

    if (std::getline(stream, token, ':')) {
        fname = token;
        while (std::getline(stream, token, '/')) {
            /* TODO: handle errors */
            const char *s = token.c_str();
            unsigned int line = strtol(s, &endptr, 10);
            if ((errno == ERANGE) || (endptr == s) || (*endptr != '\0')) {
                return false;
            }
            lines.push_back(line);
        }
    }

    return true;
}
struct SysPointers {
  Module *module;
  Interpreter *interpreter;
  KleeHandler *handler;
  int pArgc;
  char **pArgv;
  char **pEnvp;
};

int master(int argc, char **argv, char **envp);
void worker(int argc, char **argv, char **envp);

int executeWorker(int argc, char **argv, char **envp, 
	char** workList, char* prefix, unsigned int count,
  int explorationDepth=0, int mode=NO_MODE, std::string searchMode="DFS");

std::vector<std::string> split(const std::string& s, char delimiter)
{
  std::vector<std::string> tokens;
  std::string token;
  std::istringstream tokenStream(s);
  while (std::getline(tokenStream, token, delimiter)) {
    tokens.push_back(token);
  }
  return tokens;
}

void parseSkippingParameter(
  Module *module,
  std::string parameter,
  std::vector<Interpreter::SkippedFunctionOption> &result) {
  std::istringstream stream(parameter);
  std::string token;
  std::string fname;

  while (std::getline(stream, token, ',')) {
    std::vector<unsigned int> lines;
    if (!parseNameLineOption(token, fname, lines)) {
        klee_error("skip-function option: invalid parameter: %s", token.c_str());
    }
    Function *f = module->getFunction(fname);
    std::cout << "Skipping Function: "<<fname<<std::endl;
	  if (!f) {
      klee_error("skip-function option: '%s' not found in module.", fname.c_str());
    }

  	if (!f->getReturnType()->isVoidTy()) {
	    fname = std::string("__wrap_") + fname;
	  }
    result.push_back(Interpreter::SkippedFunctionOption(fname, lines));
  }
}

void parseInlinedFunctions(
    Module *module,
    std::string parameter,
    std::vector<std::string> &result) {
  std::istringstream stream(parameter);
  std::string fname;

  while (std::getline(stream, fname, ',')) {
    if (!module->getFunction(fname)) {
      klee_error("inline option: '%s' not found in module.", fname.c_str());
    }
    result.push_back(fname);
  }
}

void parseErrorLocationParameter(std::string parameter, std::map<std::string, std::vector<unsigned> > &result) {
    std::istringstream stream(parameter);
    std::string token;
    std::string fname;

    while (std::getline(stream, token, ',')) {
        std::vector<unsigned int> lines;
        if (!parseNameLineOption(token, fname, lines)) {
            klee_error("error-location option: invalid parameter: %s", token.c_str());
        }
        result.insert(std::pair<std::string, std::vector<unsigned> >(fname, lines));
    }
}

void timeOutCheck() {
  if(timeOut!=0) {
   sleep(timeOut);
  } else { 
    sleep(86400);
  }
  //MPI_Abort(MPI_COMM_WORLD, -1);
  char dummy;
  MPI_Send(&dummy, 1, MPI_CHAR, 0, TIMEOUT, MPI_COMM_WORLD);
}

int main(int argc, char **argv, char **envp) {
  atexit(llvm_shutdown);  // Call llvm_shutdown() on exit.

  llvm::InitializeNativeTarget();

  parseArguments(argc, argv);
  sys::PrintStackTraceOnErrorSignal();

  sys::SetInterruptFunction(interrupt_handle);

	/*MPI Parallel Code should go here*/
	MPI_Init(NULL, NULL);

	int world_rank;
	MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
	//master rank 
	if(world_rank == 0) {
  	master(argc, argv, envp);
	} else if(world_rank == 1) {
  	timeOutCheck();
	} else { //workers
  	worker(argc, argv, envp);
	}

	MPI_Finalize();
	return 0;
}

std::string getNewSearch() {
  if(searchPolicy == "BFS") {
    return "BFS";
  } else if (searchPolicy == "DFS") {
    return "DFS";
  } else if (searchPolicy == "RAND") {
    return "RAND";
  } else if (searchPolicy == "COVNEW") {
    return "COVNEW";
  } else {
		return "DFS";
	}
}

int master(int argc, char **argv, char **envp) {

  //setting up the workers 
  int num_cores;
  MPI_Comm_size(MPI_COMM_WORLD, &num_cores);
	std::ofstream masterLog;
	masterLog.open("log_master_"+OutputDir);
	if(phase1Depth == 0) {
		char buf[256];
		time_t t[2];
		t[0] = time(NULL);
		strftime(buf, sizeof(buf), "Started: %Y-%m-%d %H:%M:%S\n", localtime(&t[0]));
    masterLog<<buf;
		//used with 3 cores
		char dummychar;
		MPI_Status status3;
		MPI_Status status4;
		MPI_Send(&dummychar, 1, MPI_CHAR, 2, NORMAL_TASK, MPI_COMM_WORLD);
		MPI_Recv(&dummychar, 1, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status3);
		if(status3.MPI_TAG == FINISH) {
			masterLog << "MASTER_ELAPSED Normal Mode \n";
			if(FLUSH) masterLog.flush();
			MPI_Send(&dummychar, 1, MPI_CHAR, 2, KILL, MPI_COMM_WORLD);
			MPI_Recv(&dummychar, 1, MPI_CHAR, 2, KILL_COMP, MPI_COMM_WORLD, &status4);
		  masterLog.close();
		  MPI_Abort(MPI_COMM_WORLD, -1);
		} else if(status3.MPI_TAG == TIMEOUT) {
			masterLog << "MASTER_ELAPSED Timeout: \n";
		  masterLog.close();
		  MPI_Abort(MPI_COMM_WORLD, -1);
		} else if(status3.MPI_TAG == BUG_FOUND) {
			masterLog << "WORKER->MASTER:  BUG FOUND:"<<status3.MPI_SOURCE<<"\n";
			t[1] = time(NULL);
			strcpy(buf, "Elapsed: ");
			strcpy(format_tdiff(buf, t[1] - t[0]), "\n");
			masterLog<<buf;
			masterLog.close();
			//MPI_Send(&dummychar, 1, MPI_CHAR, 2, KILL, MPI_COMM_WORLD);
			//MPI_Recv(&dummychar, 1, MPI_CHAR, 2, KILL_COMP, MPI_COMM_WORLD, &status4);
		  MPI_Abort(MPI_COMM_WORLD, -1);
		}

	} else {	

		std::string ErrorMsg;
		Module *mainModule = 0;
	#if LLVM_VERSION_CODE < LLVM_VERSION(3, 5)
		OwningPtr<MemoryBuffer> BufferPtr;
		error_code ec=MemoryBuffer::getFileOrSTDIN(InputFile.c_str(), BufferPtr);
		if (ec) {
			klee_error("error loading program '%s': %s", InputFile.c_str(),
								 ec.message().c_str());
		} 

		mainModule = getLazyBitcodeModule(BufferPtr.get(), getGlobalContext(), &ErrorMsg);

		if (mainModule) {
			if (mainModule->MaterializeAllPermanently(&ErrorMsg)) {
				delete mainModule;
				mainModule = 0;
			} 
		}   
		if (!mainModule)
			klee_error("error loading program '%s': %s", InputFile.c_str(),
								 ErrorMsg.c_str());
	#else
		auto Buffer = MemoryBuffer::getFileOrSTDIN(InputFile.c_str());
		if (!Buffer)
			klee_error("error loading program '%s': %s", InputFile.c_str(),
								 Buffer.getError().message().c_str());
			
		auto mainModuleOrError = getLazyBitcodeModule(Buffer->get(), getGlobalContext());

		if (!mainModuleOrError) {
			klee_error("error loading program '%s': %s", InputFile.c_str(),
								 mainModuleOrError.getError().message().c_str());
		} 
		else {
			// The module has taken ownership of the MemoryBuffer so release it
			// from the std::unique_ptr
			Buffer->release();
		} 
			
		mainModule = *mainModuleOrError;
		if (auto ec = mainModule->materializeAllPermanently()) {
			klee_error("error loading program '%s': %s", InputFile.c_str(),
								 ec.message().c_str());
		} 
	#endif
	 
		if (WithPOSIXRuntime || WithSymArgsRuntime) {
			int r = initEnv(mainModule);
			if (r != 0)
				return r;
		}

		std::string LibraryDir = KleeHandler::getRunTimeLibraryPath(argv[0]);
		Interpreter::ModuleOptions Opts(LibraryDir.c_str(), EntryPoint,
																		/*Optimize=*/OptimizeModule,
																		/*CheckDivZero=*/CheckDivZero,
																		/*CheckOvershift=*/CheckOvershift);

		switch (Libc) {
		case NoLibc: /* silence compiler warning */
			break;

		case KleeLibc: {
			// FIXME: Find a reasonable solution for this.
			SmallString<128> Path(Opts.LibraryDir);
	#if LLVM_VERSION_CODE >= LLVM_VERSION(3,3)
			llvm::sys::path::append(Path, "klee-libc.bc");
	#else
			llvm::sys::path::append(Path, "libklee-libc.bca");
	#endif
			mainModule = klee::linkWithLibrary(mainModule, Path.c_str());
			assert(mainModule && "unable to link with klee-libc");
			break;
		}

		case UcLibc:
			mainModule = linkWithUclibc(mainModule, LibraryDir);
			break;
		}

		if (WithPOSIXRuntime) {
			SmallString<128> Path(Opts.LibraryDir);
			llvm::sys::path::append(Path, "libkleeRuntimePOSIX.bca");
			klee_message("NOTE: Using model: %s", Path.c_str());
			mainModule = klee::linkWithLibrary(mainModule, Path.c_str());
			assert(mainModule && "unable to link with simple model");
		}

		std::vector<std::string>::iterator libs_it;
		std::vector<std::string>::iterator libs_ie;
		for (libs_it = LinkLibraries.begin(), libs_ie = LinkLibraries.end();
						libs_it != libs_ie; ++libs_it) {
			const char * libFilename = libs_it->c_str();
			klee_message("Linking in library: %s.\n", libFilename);
			mainModule = klee::linkWithLibrary(mainModule, libFilename);
		}
		
		// Get the desired main function.  klee_main initializes uClibc
		// locale and other data and then calls main.
		Function *mainFn = mainModule->getFunction(EntryPoint);
		if (!mainFn) {
			klee_error("'%s' function not found in module.", EntryPoint.c_str());
		}

		std::vector<Interpreter::SkippedFunctionOption> skippingOptions;
		parseSkippingParameter(mainModule, SkippedFunctions, skippingOptions);

		std::vector<std::string> inlinedFunctions;
		parseInlinedFunctions(mainModule, InlinedFunctions, inlinedFunctions);

		std::map<std::string, std::vector<unsigned> > errorLocationOptions;
		parseErrorLocationParameter(ErrorLocation, errorLocationOptions);

		// FIXME: Change me to std types.
		int pArgc;
		char **pArgv;
		char **pEnvp;
		if (Environ != "") {
			std::vector<std::string> items;
			std::ifstream f(Environ.c_str());
			if (!f.good())
				klee_error("unable to open --environ file: %s", Environ.c_str());
			while (!f.eof()) {
				std::string line;
				std::getline(f, line);
				line = strip(line);
				if (!line.empty())
					items.push_back(line);
			}
			f.close();
			pEnvp = new char *[items.size()+1];
			unsigned i=0;
			for (; i != items.size(); ++i)
				pEnvp[i] = strdup(items[i].c_str());
			pEnvp[i] = 0;
		} else {
			pEnvp = envp;
		}

		pArgc = InputArgv.size() + 1;
		pArgv = new char *[pArgc];
		for (unsigned i=0; i<InputArgv.size()+1; i++) {
			std::string &arg = (i==0 ? InputFile : InputArgv[i-1]);
			unsigned size = arg.size() + 1;
			char *pArg = new char[size];

			std::copy(arg.begin(), arg.end(), pArg);
			pArg[size - 1] = 0;

			pArgv[i] = pArg;
		}

		Interpreter::InterpreterOptions IOpts;
		IOpts.MakeConcreteSymbolic = MakeConcreteSymbolic;
		IOpts.skippedFunctions = skippingOptions;
		IOpts.inlinedFunctions = inlinedFunctions;
		IOpts.errorLocations = errorLocationOptions;
		IOpts.maxErrorCount = MaxErrorCount;
		KleeHandler *handler = new KleeHandler(pArgc, pArgv);
		Interpreter *interpreter =
			theInterpreter = Interpreter::create(IOpts, handler);
		handler->setInterpreter(interpreter);
		
		for (int i=0; i<argc; i++) {
			handler->getInfoStream() << argv[i] << (i+1<argc ? " ":"\n");
		}
		handler->getInfoStream() << "PID: " << getpid() << "\n";

		const Module *finalModule =
			interpreter->setModule(mainModule, Opts);
		externalsAndGlobalsCheck(finalModule);

		std::string output_dir_file;
		std::string pthfile;

		char buf[256];
		time_t t[2];
		t[0] = time(NULL);
		strftime(buf, sizeof(buf), "Started: %Y-%m-%d %H:%M:%S\n", localtime(&t[0]));
		handler->getInfoStream() << buf;
		handler->getInfoStream().flush();

		interpreter->setExplorationDepth(phase1Depth);

		interpreter->setSearchMode("DFS");
		pthfile = handler->getOutputDir()+"_pathFile_"+std::to_string(0);
		interpreter->setPathFile(pthfile);

		output_dir_file = handler->getOutputDir();
		interpreter->setBrHistFile(output_dir_file+"_br_hist");
		interpreter->setLogFile(output_dir_file+"_log_file");
		int world_rank;
		MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
		std::cout<<"DMap World Rank: "<<world_rank<<" File: " <<output_dir_file<<std::endl;
		//std::cout.flush();
		
		char** workList;
		std::vector<unsigned int> pathSizes;

		workList = interpreter->runFunctionAsMain2(mainFn, pArgc, pArgv, pEnvp, pathSizes);
	 
		std::vector<unsigned char> dummyprefix;
		std::deque<unsigned char> dummyWL;
		std::deque<unsigned int> freeList;
		std::deque<unsigned int> offloadActiveList;
		std::deque<unsigned int> busyList;
		std::deque<unsigned int> offloadReadyList;
		MPI_Status status2;
		dummyWL.resize(phase1Depth);
		//std::ofstream masterLog;
		//masterLog.open("log_master_"+OutputDir);
		masterLog << "MASTER_START \n";
	 
		//*************Seeding the worker*************
		int currRank = 2;
		//auto wListIt = workList.begin();
		int whileCnt = (num_cores-2)<pathSizes.size()?num_cores-2:pathSizes.size();

		int cnt=0;
		while(cnt<whileCnt) {
			std::cout << "Starting worker: "<<currRank<<"\n";
			masterLog << "MASTER->WORKER: START_WORK ID:"<<currRank<<"\n";
			if(FLUSH) masterLog.flush();
			MPI_Send(&(workList[cnt][0]), pathSizes[cnt], MPI_CHAR, currRank, START_PREFIX_TASK, 
					MPI_COMM_WORLD);
			busyList.push_back(currRank);
			++currRank;
			++cnt;
		}
	 
		//If worklist size is smaller than cores, kill the rest of the processes
		while(currRank < num_cores) {
			if(!lb) {
				char dummy2;
				MPI_Send(&dummy2, 1, MPI_CHAR, currRank, KILL, MPI_COMM_WORLD);
				std::cout << "Killing(not required) worker: "<<currRank<<"\n";
				masterLog << "MASTER->WORKER: KILL ID:"<<currRank<<"\n";
			}
			freeList.push_back(currRank);
			++currRank;
		}

		//receive FINISH/BUG FOUND/OFFLOAD READY/NOT READY messages 
		//from workers and offload further work
		char dummyRecv;
		MPI_Status status;
		while(cnt < pathSizes.size()) {
			MPI_Recv(&dummyRecv, 1, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
			if(status.MPI_TAG == FINISH) {
				for(auto it = busyList.begin(); it != busyList.end(); ++it) {
					if (*it == status.MPI_SOURCE) {
						busyList.erase(it);
						break;
					 }
				}
				//also remove from offloadReadyList if it exists
				for(auto it = offloadActiveList.begin(); it != offloadActiveList.end(); ++it) {
					if (*it == status.MPI_SOURCE) {
						offloadActiveList.erase(it);
						break;
					}
				}

				masterLog << "WORKER->MASTER: FINISH ID:"<<status.MPI_SOURCE<<"\n";
				if(FLUSH) masterLog.flush();
				MPI_Send(&(workList[cnt][0]), pathSizes[cnt], MPI_CHAR, status.MPI_SOURCE,
					START_PREFIX_TASK, MPI_COMM_WORLD);
				masterLog << "MASTER->WORKER: START_WORK ID:"<<status.MPI_SOURCE<<"\n";
				if(FLUSH) masterLog.flush();

				busyList.push_back(status.MPI_SOURCE);
				cnt++;
			} else if(status.MPI_TAG == BUG_FOUND) {
				t[1] = time(NULL);
				strcpy(buf, "Elapsed: ");
				strcpy(format_tdiff(buf, t[1] - t[0]), "\n");
				masterLog<<buf;
				masterLog.close();

				char dummy;
				for(int x=2; x<num_cores; ++x) {
					MPI_Send(&dummy, 1, MPI_CHAR, x, KILL, MPI_COMM_WORLD);
				}
			} else if(status.MPI_TAG == READY_TO_OFFLOAD) {
				//masterLog << "WORKER->MASTER: READY TO OFFLOAD:"<<status.MPI_SOURCE<<"\n";
				offloadReadyList.push_back(status.MPI_SOURCE);
			} else if(status.MPI_TAG == NOT_READY_TO_OFFLOAD) {
				bool found2Erase=false;
				//masterLog << "WORKER->MASTER: NOT READY TO OFFLOAD:"<<status.MPI_SOURCE<<"\n";
				for(auto it=offloadReadyList.begin(); it!=offloadReadyList.end(); ++it) {
					if(*it==status.MPI_SOURCE) {
						offloadReadyList.erase(it);
						found2Erase=true;
						break;
					}
				}
				assert(found2Erase);
			} else {
				//should not see any tags here
				bool ok = false;
				(void) ok;
				assert(ok && "MASTER received an illegal tag");
			}

		}

		std::cout << "Done with all prefixes\n";
		masterLog << "MASTER: DONE_WITH_ALL_PREFIXES\n";
		delete workList;
		if(FLUSH) masterLog.flush();
		bool offloadActive = false;
		//masterLog.flush();
		while(true) {
			MPI_Status status;
			int flag=false, count;
			//char *buffer;
			//see what the workers are saying
			MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);

			if(flag) {
				MPI_Get_count(&status, MPI_CHAR, &count);
				char buffer[count];
				MPI_Recv(buffer, count, MPI_CHAR, status.MPI_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
				//masterLog << "RECVD something: "<<status.MPI_SOURCE<<" "<<count <<"\n";
				//masterLog.flush();
				if(status.MPI_TAG == BUG_FOUND) {
					masterLog << "WORKER->MASTER:  BUG FOUND:"<<status.MPI_SOURCE<<"\n";
					t[1] = time(NULL);
					strcpy(buf, "Elapsed: ");
					strcpy(format_tdiff(buf, t[1] - t[0]), "\n");
					masterLog<<buf;
					masterLog.close();
					char dummy;
					MPI_Abort(MPI_COMM_WORLD, -1);
				} else if(status.MPI_TAG == FINISH) {
					bool ffound=0;
					for(auto it=freeList.begin(); it!=freeList.end(); ++it) {
						if(*it == status.MPI_SOURCE) {
							ffound=1;
							break;
						}
					}
					if(!ffound) freeList.push_back(status.MPI_SOURCE);

					for(auto it = busyList.begin(); it != busyList.end(); ++it) {
						if (*it == status.MPI_SOURCE) {
							busyList.erase(it);
							break;
						 }
					}

					//also remove from offloadReadyList if it exists
					for(auto it = offloadActiveList.begin(); it != offloadActiveList.end(); ++it) {
						if (*it == status.MPI_SOURCE) {
							offloadActiveList.erase(it);
              offloadActive = false;
							break;
						}
					}
					
          for(auto it=offloadReadyList.begin(); it!=offloadReadyList.end(); ++it) {
						if(*it==status.MPI_SOURCE) {
							offloadReadyList.erase(it);
              break;
						}
					}

					masterLog << "WORKER->MASTER: FINISH ID:"<<status.MPI_SOURCE<<"\n";
					masterLog << "WORKER->MASTER: FREELIST SIZE:"<<freeList.size()<<"\n";
					if(FLUSH) masterLog.flush();
					//if all workers finish then shut down the system
					if(freeList.size() == (num_cores-2)) {
						masterLog << "MASTER: ALL WORKERS FINISHED \n";
						if(FLUSH) masterLog.flush();
						//Kill all the workers
						char dummy;
						for(int x=2; x<num_cores; ++x) {
							MPI_Send(&dummy, 1, MPI_CHAR, x, KILL, MPI_COMM_WORLD);
						}

						masterLog << "MASTER_ELAPSED: \n";
						t[1] = time(NULL);
						strcpy(buf, "Elapsed: ");
						strcpy(format_tdiff(buf, t[1] - t[0]), "\n");
						masterLog<<buf;
						masterLog.close();

						for(int x=2; x<num_cores; ++x) {
							MPI_Recv(&dummy, 1, MPI_CHAR, x, KILL_COMP, MPI_COMM_WORLD, &status2);
						}
						MPI_Abort(MPI_COMM_WORLD, -1);
					}
				} else if(status.MPI_TAG == TIMEOUT) {
					char dummy;
					for(int x=2; x<num_cores; ++x) {
						MPI_Send(&dummy, 1, MPI_CHAR, x, KILL, MPI_COMM_WORLD);
					}
            
          for(int x=2; x<num_cores; ++x) {
              MPI_Recv(&dummy, 1, MPI_CHAR, x, KILL_COMP, MPI_COMM_WORLD, &status2);
          } 
          MPI_Abort(MPI_COMM_WORLD, -1);

				} else if(status.MPI_TAG == READY_TO_OFFLOAD) {
					//masterLog << "WORKER->MASTER: READY TO OFFLOAD:"<<status.MPI_SOURCE<<"\n";
					offloadReadyList.push_back(status.MPI_SOURCE);
				} else if(status.MPI_TAG == NOT_READY_TO_OFFLOAD) {
					bool found2Erase=false;
					//masterLog << "WORKER->MASTER: NOT READY TO OFFLOAD:"<<status.MPI_SOURCE<<"\n";
					for(auto it=offloadReadyList.begin(); it!=offloadReadyList.end(); ++it) {
						if(*it==status.MPI_SOURCE) {
							offloadReadyList.erase(it);
							found2Erase=true;
							break;
						}
					}
					//assert(found2Erase);
				} else if(status.MPI_TAG == OFFLOAD_RESP) {
					masterLog << "WORKER->MASTER: OFFLOAD RCVD ID:"<<status.MPI_SOURCE<<" Length:"<<count<<"\n";
					if(FLUSH) masterLog.flush();

					//Removing the offload active list worker 
					for(auto it = offloadActiveList.begin(); it != offloadActiveList.end(); ++it) {
						if (*it == status.MPI_SOURCE) {
							offloadActiveList.erase(it);
							break;
						}
					}

					//Send the offloaded work to the free worker 
					if(count>4) {
						//something should exist in free list
						assert(freeList.size() > 0);
						unsigned int pickedWorker = freeList.front();
						masterLog << "MASTER->WORKER: PREFIX_TASK_SEND ID:"<<pickedWorker<<" Length:"<<count<<"\n";
						MPI_Send(buffer, count, MPI_CHAR, pickedWorker, START_PREFIX_TASK, MPI_COMM_WORLD);
						masterLog << "MASTER->WORKER: START_WORK ID:"<<pickedWorker<<"\n";
						//pushing the worker busy list
						busyList.push_back(pickedWorker);
						freeList.pop_front();
					}
					offloadActive = false;
				} else {
					//should not see any tags here
					std::cout << "ILLEGAL TAG: "<<status.MPI_TAG<<" "<<status.MPI_SOURCE<<"\n";
					if(FLUSH) std::cout.flush();
					bool ok = false;
					(void) ok;
					assert(ok && "MASTER received an illegal tag");
				}
			}

			//if some workers are ready to offload and freelist has some workers
			//offload some stuff
			if(lb && (freeList.size()>0) && (freeList.size()<(num_cores-2))
				 && (offloadReadyList.size()>0) && !offloadActive) {

				//pick out the worker that has been busy the longest and to whom an
				//offload request in not yet sent
				bool foundWorker2Offload = false;
				unsigned int worker2offload;
				for(auto it = offloadReadyList.begin(); it != offloadReadyList.end(); ++it) {
					bool offloadAlreadySent = false;
					for(auto it2 = offloadActiveList.begin(); it2 < offloadActiveList.end(); ++it2) {
						if(*it == *it2) {
							offloadAlreadySent = true;
							break;
						}
					}
					if(!offloadAlreadySent) {
						foundWorker2Offload = true;
						worker2offload = *it;
						offloadActiveList.push_back(worker2offload);
						break;
					}
				}
				//found a valid busy worker
				if(foundWorker2Offload) {
					MPI_Status offloadStatus;
					char dummyBuff;
					MPI_Send(&dummyBuff, 1, MPI_CHAR, worker2offload, OFFLOAD, MPI_COMM_WORLD);
					masterLog << "MASTER->WORKER: OFFLOAD_SENT ID:"<<worker2offload<<"\n";
					if(FLUSH) masterLog.flush();
					offloadActive = true;
				}
			}
		}
		
		// Free all the args.
		for (unsigned i=0; i<InputArgv.size()+1; i++)
			delete[] pArgv[i];
		delete[] pArgv;
		delete interpreter;

		t[1] = time(NULL);
		strftime(buf, sizeof(buf), "Finished: %Y-%m-%d %H:%M:%S\n", localtime(&t[1]));
		handler->getInfoStream() << buf;

		strcpy(buf, "Elapsed: ");
		strcpy(format_tdiff(buf, t[1] - t[0]), "\n");
		handler->getInfoStream() << buf;

	#if LLVM_VERSION_CODE < LLVM_VERSION(3, 5)
		// FIXME: This really doesn't look right
		// This is preventing the module from being
		// deleted automatically
		BufferPtr.take();
	#endif
			
		//remove the br_hist file path files log files and directory
		if(ENABLE_CLEANUP) {
			std::string brhist = output_dir_file+"_br_hist";
			const char * c = brhist.c_str();
			std::string logfile = output_dir_file+"_log_file";
			const char * d = logfile.c_str();
			const char * e = output_dir_file.c_str();
			std::string pathfile = pthfile;
			const char * f = pthfile.c_str();
			std::string a1 = output_dir_file+"/assembly.ll";
			const char * a = a1.c_str();
			std::string b1 = output_dir_file+"/info";
			const char * b = b1.c_str();
			std::string x1 = output_dir_file+"/messages.txt";
			const char * x = x1.c_str();
			std::string z1 = output_dir_file+"/warnings.txt";
			const char * z = z1.c_str();
			std::string z2 = output_dir_file+"/sa.log";
			const char * z3 = z2.c_str();
			remove(c);
			remove(d);
			remove(f);
			remove(a);
			remove(b);
			remove(x);
			remove(z);
			remove(z3);
			rmdir(e);
			std::cout << "Deleting File: " << a1 << "\n";
		}

		delete handler;
		return 0;
	}
}

void worker(int argc, char **argv, char **envp) {
  int world_rank;
  char result;
  char** dummyworkList;

  while(true) {
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    //trying to check the TAG of incoming message
    MPI_Status status;
    MPI_Probe(0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
    int count;
    MPI_Get_count(&status, MPI_CHAR, &count);

    if(status.MPI_TAG == KILL) {
      std::deque<unsigned char> recv_prefix;
      recv_prefix.resize(phase1Depth);
      MPI_Recv(&recv_prefix[0], phase1Depth, MPI_CHAR, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
      std::cout << "Killing Process: "<<world_rank<<"\n";
      return;

    } else if(status.MPI_TAG == START_PREFIX_TASK) {
      //std::vector<unsigned char> recv_prefix;
      //recv_prefix.resize(count);
      char* recv_prefix = (char*)malloc((count)*sizeof(char)); 
      MPI_Recv(recv_prefix, count, MPI_CHAR, 0, START_PREFIX_TASK, MPI_COMM_WORLD, &status);
      std::cout << "Process: "<<world_rank<<" Prefix Task: Length:"<<count<<" ";
      //std::cout.flush();
      for(unsigned int x=count-10;x<count;x++) {
        std::cout<<recv_prefix[x];
      }
      std::cout<<"\n";
      executeWorker(argc, argv, envp, dummyworkList, recv_prefix, count, phase2Depth, 
          PREFIX_MODE, getNewSearch());
      std::cout << "Finish: " << world_rank << std::endl;
      delete recv_prefix;
      //MPI_Send(&result, 1, MPI_CHAR, 0, FINISH, MPI_COMM_WORLD);
      MPI_Send(&result, 1, MPI_CHAR, 0, KILL_COMP, MPI_COMM_WORLD);
      return;
		} else if(status.MPI_TAG == NORMAL_TASK) {
      std::cout << "Process: "<<world_rank<<" Normal Task "<<"Prefix Depth: "<<phase2Depth<<"\n";
      char* recv_prefix = (char*)malloc((count+1)*sizeof(char)); 
      MPI_Recv(recv_prefix, count, MPI_CHAR, 0, NORMAL_TASK, MPI_COMM_WORLD, &status);
      executeWorker(argc, argv, envp, dummyworkList, recv_prefix, count, phase2Depth, 
          NO_MODE, getNewSearch());
      MPI_Send(&result, 1, MPI_CHAR, 0, FINISH, MPI_COMM_WORLD);

    } else if(status.MPI_TAG == OFFLOAD) {
      int count, buffer;
      MPI_Get_count(&status, MPI_INT, &count);
      MPI_Recv(&buffer, count, MPI_INT, MASTER_NODE, OFFLOAD, MPI_COMM_WORLD, &status);
      std::vector<unsigned char> packet2send;
      packet2send.push_back('x');
      MPI_Send(&packet2send[0], packet2send.size(), MPI_CHAR, 0, OFFLOAD_RESP, MPI_COMM_WORLD);

    } 
  }
}

int executeWorker(int argc, char **argv, char **envp, 
    char** workList, char* prefix, unsigned int count, 
		int explorationDepth, int mode, std::string searchMode) {

  std::string ErrorMsg;
  Module *mainModule = 0;
#if LLVM_VERSION_CODE < LLVM_VERSION(3, 5)
  OwningPtr<MemoryBuffer> BufferPtr;
  error_code ec=MemoryBuffer::getFileOrSTDIN(InputFile.c_str(), BufferPtr);
  if (ec) {
    klee_error("error loading program '%s': %s", InputFile.c_str(),
               ec.message().c_str());
  }

  mainModule = getLazyBitcodeModule(BufferPtr.get(), getGlobalContext(), &ErrorMsg);

  if (mainModule) {
    if (mainModule->MaterializeAllPermanently(&ErrorMsg)) {
      delete mainModule;
      mainModule = 0;
    }
  }
  if (!mainModule)
    klee_error("error loading program '%s': %s", InputFile.c_str(),
               ErrorMsg.c_str());
#else
  auto Buffer = MemoryBuffer::getFileOrSTDIN(InputFile.c_str());
  if (!Buffer)
    klee_error("error loading program '%s': %s", InputFile.c_str(),
               Buffer.getError().message().c_str());

  auto mainModuleOrError = getLazyBitcodeModule(Buffer->get(), getGlobalContext());

  if (!mainModuleOrError) {
    klee_error("error loading program '%s': %s", InputFile.c_str(),
               mainModuleOrError.getError().message().c_str());
  }
  else {
    // The module has taken ownership of the MemoryBuffer so release it
    // from the std::unique_ptr
    Buffer->release();
  }

  mainModule = *mainModuleOrError;
  if (auto ec = mainModule->materializeAllPermanently()) {
    klee_error("error loading program '%s': %s", InputFile.c_str(),
               ec.message().c_str());
  }
#endif

  if (WithPOSIXRuntime || WithSymArgsRuntime) {
    int r = initEnv(mainModule);
    if (r != 0)
      return r;
  }

  std::string LibraryDir = KleeHandler::getRunTimeLibraryPath(argv[0]);
  Interpreter::ModuleOptions Opts(LibraryDir.c_str(), EntryPoint,
                                  /*Optimize=*/OptimizeModule,
                                  /*CheckDivZero=*/CheckDivZero,
                                  /*CheckOvershift=*/CheckOvershift);

  switch (Libc) {
  case NoLibc: /* silence compiler warning */
    break;

  case KleeLibc: {
    // FIXME: Find a reasonable solution for this.
    SmallString<128> Path(Opts.LibraryDir);
#if LLVM_VERSION_CODE >= LLVM_VERSION(3,3)
    llvm::sys::path::append(Path, "klee-libc.bc");
#else
    llvm::sys::path::append(Path, "libklee-libc.bca");
#endif
    mainModule = klee::linkWithLibrary(mainModule, Path.c_str());
    assert(mainModule && "unable to link with klee-libc");
    break;
  }

  case UcLibc:
    mainModule = linkWithUclibc(mainModule, LibraryDir);
    break;
  }

  if (WithPOSIXRuntime) {
    SmallString<128> Path(Opts.LibraryDir);
    llvm::sys::path::append(Path, "libkleeRuntimePOSIX.bca");
    klee_message("NOTE: Using model: %s", Path.c_str());
    mainModule = klee::linkWithLibrary(mainModule, Path.c_str());
    assert(mainModule && "unable to link with simple model");
  }

  std::vector<std::string>::iterator libs_it;
  std::vector<std::string>::iterator libs_ie;
  for (libs_it = LinkLibraries.begin(), libs_ie = LinkLibraries.end();
          libs_it != libs_ie; ++libs_it) {
    const char * libFilename = libs_it->c_str();
    klee_message("Linking in library: %s.\n", libFilename);
    mainModule = klee::linkWithLibrary(mainModule, libFilename);
  }

  // Get the desired main function.  klee_main initializes uClibc
  // locale and other data and then calls main.
  Function *mainFn = mainModule->getFunction(EntryPoint);
  if (!mainFn) {
    klee_error("'%s' function not found in module.", EntryPoint.c_str());
  }

  std::vector<Interpreter::SkippedFunctionOption> skippingOptions;
  parseSkippingParameter(mainModule, SkippedFunctions, skippingOptions);

  std::vector<std::string> inlinedFunctions;
  parseInlinedFunctions(mainModule, InlinedFunctions, inlinedFunctions);

  std::map<std::string, std::vector<unsigned> > errorLocationOptions;
  parseErrorLocationParameter(ErrorLocation, errorLocationOptions);

	// FIXME: Change me to std types.
	int pArgc;
	char **pArgv;
	char **pEnvp;
	if (Environ != "") {
	  std::vector<std::string> items;
	  std::ifstream f(Environ.c_str());
	  if (!f.good())
	 	 klee_error("unable to open --environ file: %s", Environ.c_str());
	  while (!f.eof()) {
	 	 std::string line;
	 	 std::getline(f, line);
	 	 line = strip(line);
	 	 if (!line.empty())
	 		 items.push_back(line);
	  }
	  f.close();
	  pEnvp = new char *[items.size()+1];
	  unsigned i=0;
	  for (; i != items.size(); ++i)
	 	 pEnvp[i] = strdup(items[i].c_str());
	  pEnvp[i] = 0;
	} else {
	  pEnvp = envp;
	}

	pArgc = InputArgv.size() + 1;
	pArgv = new char *[pArgc];
	for (unsigned i=0; i<InputArgv.size()+1; i++) {
	  std::string &arg = (i==0 ? InputFile : InputArgv[i-1]);
	  unsigned size = arg.size() + 1;
	  char *pArg = new char[size];

	  std::copy(arg.begin(), arg.end(), pArg);
	  pArg[size - 1] = 0;

	  pArgv[i] = pArg;
	}

	Interpreter::InterpreterOptions IOpts;
	IOpts.MakeConcreteSymbolic = MakeConcreteSymbolic;
	IOpts.skippedFunctions = skippingOptions;
	IOpts.inlinedFunctions = inlinedFunctions;
	IOpts.errorLocations = errorLocationOptions;
	IOpts.maxErrorCount = MaxErrorCount;
	KleeHandler *handler = new KleeHandler(pArgc, pArgv);
	Interpreter *interpreter =
	  theInterpreter = Interpreter::create(IOpts, handler);
	handler->setInterpreter(interpreter);

	for (int i=0; i<argc; i++) {
	  handler->getInfoStream() << argv[i] << (i+1<argc ? " ":"\n");
	}
	handler->getInfoStream() << "PID: " << getpid() << "\n";

	const Module *finalModule =
	  interpreter->setModule(mainModule, Opts);
	externalsAndGlobalsCheck(finalModule);

	std::string output_dir_file;
	std::string pthfile;

	char buf[256];
	time_t t[2];
	t[0] = time(NULL);
	strftime(buf, sizeof(buf), "Started: %Y-%m-%d %H:%M:%S\n", localtime(&t[0]));
	handler->getInfoStream() << buf;
	handler->getInfoStream().flush();

	interpreter->setExplorationDepth(explorationDepth);

	if(mode == PREFIX_MODE) {
	  interpreter->setUpperBound(prefix);
	  interpreter->setLowerBound(prefix);
	  interpreter->enablePrefixChecking();
    interpreter->setTestPrefixDepth(count);
	}

  if(mode == NO_MODE) {
    interpreter->setTestPrefixDepth(0);
  }
	
	int world_rank;
	MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);

	interpreter->enableLoadBalancing(lb);
	interpreter->setSearchMode(searchMode);
	pthfile = handler->getOutputDir()+"_pathFile_"+std::to_string(world_rank);
	interpreter->setPathFile(pthfile);

	output_dir_file = handler->getOutputDir();
	interpreter->setBrHistFile(output_dir_file+"_br_hist");
	interpreter->setLogFile(output_dir_file+"_log_file");
	std::cout<<"DMap World Rank: "<<world_rank<<" File: " <<output_dir_file<<std::endl;
  std::vector<unsigned int> pathSizes;
	interpreter->runFunctionAsMain2(mainFn, pArgc, pArgv, pEnvp, pathSizes);

  //time_t t;
  t[1] = time(NULL);
  strftime(buf, sizeof(buf), "Finished: %Y-%m-%d %H:%M:%S\n", localtime(&t[1]));
  handler->getInfoStream() << buf;

  strcpy(buf, "Elapsed: ");
  strcpy(format_tdiff(buf, t[1] - t[0]), "\n");
  handler->getInfoStream() << buf;


  // Free all the args.
  for (unsigned i=0; i<InputArgv.size()+1; i++)
    delete[] pArgv[i];
  delete[] pArgv;

  delete interpreter;

  uint64_t queries =
    *theStatisticManager->getStatisticByName("Queries");
  uint64_t queriesValid =
    *theStatisticManager->getStatisticByName("QueriesValid");
  uint64_t queriesInvalid =
    *theStatisticManager->getStatisticByName("QueriesInvalid");
  uint64_t queryCounterexamples =
    *theStatisticManager->getStatisticByName("QueriesCEX");
  uint64_t queryConstructs =
    *theStatisticManager->getStatisticByName("QueriesConstructs");
  uint64_t instructions =
    *theStatisticManager->getStatisticByName("Instructions");
  uint64_t forks =
    *theStatisticManager->getStatisticByName("Forks");

  handler->getInfoStream()
    << "KLEE: done: explored paths = " << 1 + forks << "\n";

  // Write some extra information in the info file which users won't
  // necessarily care about or understand.
  if (queries)
    handler->getInfoStream()
      << "KLEE: done: avg. constructs per query = "
                             << queryConstructs / queries << "\n";
  handler->getInfoStream()
    << "KLEE: done: total queries = " << queries << "\n"
    << "KLEE: done: valid queries = " << queriesValid << "\n"
    << "KLEE: done: invalid queries = " << queriesInvalid << "\n"
    << "KLEE: done: query cex = " << queryCounterexamples << "\n";

  std::stringstream stats;
  stats << "\n";
  stats << "KLEE: done: total instructions = "
        << instructions << "\n";
  stats << "KLEE: done: completed paths = "
        << handler->getNumPathsExplored() << "\n";
  stats << "KLEE: done: generated tests = "
        << handler->getNumTestCases() << "\n";

  /* these are relevant only when we have a slicing option */
  //TODO get IOptd
  /*if (!IOpts.skippedFunctions.empty()) {
    stats << "KLEE: done: recovery states = "
          << handler->getRecoveryStatesCount() << "\n";
    stats << "KLEE: done: generated slices = "
          << handler->getGeneratedSlicesCount() << "\n";
    stats << "KLEE: done: created snapshots = "
          << handler->getSnapshotsCount() << "\n";
  }*/

  bool useColors = llvm::errs().is_displayed();
  if (useColors)
    llvm::errs().changeColor(llvm::raw_ostream::GREEN,
                             /*bold=*/true,
                             /*bg=*/false);


  handler->getInfoStream() << stats.str();

  llvm::errs() << stats.str();

  if (useColors)
    llvm::errs().resetColor();

#if LLVM_VERSION_CODE < LLVM_VERSION(3, 5)
  // FIXME: This really doesn't look right
  // This is preventing the module from being
  // deleted automatically
  BufferPtr.take();
#endif

  return 0;

}
