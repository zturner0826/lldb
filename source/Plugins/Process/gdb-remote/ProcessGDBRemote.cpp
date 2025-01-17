//===-- ProcessGDBRemote.cpp ------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/lldb-python.h"
#include "lldb/Host/Config.h"

// C Includes
#include <errno.h>
#include <stdlib.h>
#ifndef LLDB_DISABLE_POSIX
#include <spawn.h>
#include <netinet/in.h>
#include <sys/mman.h>       // for mmap
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

// C++ Includes
#include <algorithm>
#include <map>

// Other libraries and framework includes

#include "lldb/Breakpoint/Watchpoint.h"
#include "lldb/Interpreter/Args.h"
#include "lldb/Core/ArchSpec.h"
#include "lldb/Core/Debugger.h"
#include "lldb/Core/ConnectionFileDescriptor.h"
#include "lldb/Host/FileSpec.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/ModuleSpec.h"
#include "lldb/Core/PluginManager.h"
#include "lldb/Core/State.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Core/StreamString.h"
#include "lldb/Core/Timer.h"
#include "lldb/Core/Value.h"
#include "lldb/Host/Symbols.h"
#include "lldb/Host/TimeValue.h"
#include "lldb/Interpreter/CommandInterpreter.h"
#include "lldb/Interpreter/CommandObject.h"
#include "lldb/Interpreter/CommandObjectMultiword.h"
#include "lldb/Interpreter/CommandReturnObject.h"
#ifndef LLDB_DISABLE_PYTHON
#include "lldb/Interpreter/PythonDataObjects.h"
#endif
#include "lldb/Symbol/ObjectFile.h"
#include "lldb/Target/DynamicLoader.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/TargetList.h"
#include "lldb/Target/ThreadPlanCallFunction.h"
#include "lldb/Target/SystemRuntime.h"
#include "lldb/Utility/PseudoTerminal.h"

// Project includes
#include "lldb/Host/Host.h"
#include "Plugins/Process/Utility/FreeBSDSignals.h"
#include "Plugins/Process/Utility/InferiorCallPOSIX.h"
#include "Plugins/Process/Utility/LinuxSignals.h"
#include "Plugins/Process/Utility/StopInfoMachException.h"
#include "Plugins/Platform/MacOSX/PlatformRemoteiOS.h"
#include "Utility/StringExtractorGDBRemote.h"
#include "GDBRemoteRegisterContext.h"
#include "ProcessGDBRemote.h"
#include "ProcessGDBRemoteLog.h"
#include "ThreadGDBRemote.h"


namespace lldb
{
    // Provide a function that can easily dump the packet history if we know a
    // ProcessGDBRemote * value (which we can get from logs or from debugging).
    // We need the function in the lldb namespace so it makes it into the final
    // executable since the LLDB shared library only exports stuff in the lldb
    // namespace. This allows you to attach with a debugger and call this
    // function and get the packet history dumped to a file.
    void
    DumpProcessGDBRemotePacketHistory (void *p, const char *path)
    {
        lldb_private::StreamFile strm;
        lldb_private::Error error (strm.GetFile().Open(path, lldb_private::File::eOpenOptionWrite | lldb_private::File::eOpenOptionCanCreate));
        if (error.Success())
            ((ProcessGDBRemote *)p)->GetGDBRemote().DumpHistory (strm);
    }
}

#define DEBUGSERVER_BASENAME    "debugserver"
using namespace lldb;
using namespace lldb_private;


namespace {

    static PropertyDefinition
    g_properties[] =
    {
        { "packet-timeout" , OptionValue::eTypeUInt64 , true , 1, NULL, NULL, "Specify the default packet timeout in seconds." },
        { "target-definition-file" , OptionValue::eTypeFileSpec , true, 0 , NULL, NULL, "The file that provides the description for remote target registers." },
        {  NULL            , OptionValue::eTypeInvalid, false, 0, NULL, NULL, NULL  }
    };
    
    enum
    {
        ePropertyPacketTimeout,
        ePropertyTargetDefinitionFile
    };
    
    class PluginProperties : public Properties
    {
    public:
        
        static ConstString
        GetSettingName ()
        {
            return ProcessGDBRemote::GetPluginNameStatic();
        }
        
        PluginProperties() :
        Properties ()
        {
            m_collection_sp.reset (new OptionValueProperties(GetSettingName()));
            m_collection_sp->Initialize(g_properties);
        }
        
        virtual
        ~PluginProperties()
        {
        }
        
        uint64_t
        GetPacketTimeout()
        {
            const uint32_t idx = ePropertyPacketTimeout;
            return m_collection_sp->GetPropertyAtIndexAsUInt64(NULL, idx, g_properties[idx].default_uint_value);
        }

        bool
        SetPacketTimeout(uint64_t timeout)
        {
            const uint32_t idx = ePropertyPacketTimeout;
            return m_collection_sp->SetPropertyAtIndexAsUInt64(NULL, idx, timeout);
        }

        FileSpec
        GetTargetDefinitionFile () const
        {
            const uint32_t idx = ePropertyTargetDefinitionFile;
            return m_collection_sp->GetPropertyAtIndexAsFileSpec (NULL, idx);
        }
    };
    
    typedef std::shared_ptr<PluginProperties> ProcessKDPPropertiesSP;
    
    static const ProcessKDPPropertiesSP &
    GetGlobalPluginProperties()
    {
        static ProcessKDPPropertiesSP g_settings_sp;
        if (!g_settings_sp)
            g_settings_sp.reset (new PluginProperties ());
        return g_settings_sp;
    }
    
} // anonymous namespace end

// TODO Randomly assigning a port is unsafe.  We should get an unused
// ephemeral port from the kernel and make sure we reserve it before passing
// it to debugserver.

#if defined (__APPLE__)
#define LOW_PORT    (IPPORT_RESERVED)
#define HIGH_PORT   (IPPORT_HIFIRSTAUTO)
#else
#define LOW_PORT    (1024u)
#define HIGH_PORT   (49151u)
#endif

#if defined(__APPLE__) && (defined(__arm__) || defined(__arm64__) || defined(__aarch64__))
static bool rand_initialized = false;

static inline uint16_t
get_random_port ()
{
    if (!rand_initialized)
    {
        time_t seed = time(NULL);

        rand_initialized = true;
        srand(seed);
    }
    return (rand() % (HIGH_PORT - LOW_PORT)) + LOW_PORT;
}
#endif

lldb_private::ConstString
ProcessGDBRemote::GetPluginNameStatic()
{
    static ConstString g_name("gdb-remote");
    return g_name;
}

const char *
ProcessGDBRemote::GetPluginDescriptionStatic()
{
    return "GDB Remote protocol based debugging plug-in.";
}

void
ProcessGDBRemote::Terminate()
{
    PluginManager::UnregisterPlugin (ProcessGDBRemote::CreateInstance);
}


lldb::ProcessSP
ProcessGDBRemote::CreateInstance (Target &target, Listener &listener, const FileSpec *crash_file_path)
{
    lldb::ProcessSP process_sp;
    if (crash_file_path == NULL)
        process_sp.reset (new ProcessGDBRemote (target, listener));
    return process_sp;
}

bool
ProcessGDBRemote::CanDebug (Target &target, bool plugin_specified_by_name)
{
    if (plugin_specified_by_name)
        return true;

    // For now we are just making sure the file exists for a given module
    Module *exe_module = target.GetExecutableModulePointer();
    if (exe_module)
    {
        ObjectFile *exe_objfile = exe_module->GetObjectFile();
        // We can't debug core files...
        switch (exe_objfile->GetType())
        {
            case ObjectFile::eTypeInvalid:
            case ObjectFile::eTypeCoreFile:
            case ObjectFile::eTypeDebugInfo:
            case ObjectFile::eTypeObjectFile:
            case ObjectFile::eTypeSharedLibrary:
            case ObjectFile::eTypeStubLibrary:
            case ObjectFile::eTypeJIT:
                return false;
            case ObjectFile::eTypeExecutable:
            case ObjectFile::eTypeDynamicLinker:
            case ObjectFile::eTypeUnknown:
                break;
        }
        return exe_module->GetFileSpec().Exists();
    }
    // However, if there is no executable module, we return true since we might be preparing to attach.
    return true;
}

//----------------------------------------------------------------------
// ProcessGDBRemote constructor
//----------------------------------------------------------------------
ProcessGDBRemote::ProcessGDBRemote(Target& target, Listener &listener) :
    Process (target, listener),
    m_flags (0),
    m_gdb_comm(false),
    m_debugserver_pid (LLDB_INVALID_PROCESS_ID),
    m_last_stop_packet (),
    m_last_stop_packet_mutex (Mutex::eMutexTypeNormal),
    m_register_info (),
    m_async_broadcaster (NULL, "lldb.process.gdb-remote.async-broadcaster"),
    m_async_thread (LLDB_INVALID_HOST_THREAD),
    m_async_thread_state(eAsyncThreadNotStarted),
    m_async_thread_state_mutex(Mutex::eMutexTypeRecursive),
    m_thread_ids (),
    m_continue_c_tids (),
    m_continue_C_tids (),
    m_continue_s_tids (),
    m_continue_S_tids (),
    m_max_memory_size (0),
    m_remote_stub_max_memory_size (0),
    m_addr_to_mmap_size (),
    m_thread_create_bp_sp (),
    m_waiting_for_attach (false),
    m_destroy_tried_resuming (false),
    m_command_sp (),
    m_breakpoint_pc_offset (0),
    m_unix_signals_sp (new UnixSignals ())
{
    m_async_broadcaster.SetEventName (eBroadcastBitAsyncThreadShouldExit,   "async thread should exit");
    m_async_broadcaster.SetEventName (eBroadcastBitAsyncContinue,           "async thread continue");
    m_async_broadcaster.SetEventName (eBroadcastBitAsyncThreadDidExit,      "async thread did exit");
    const uint64_t timeout_seconds = GetGlobalPluginProperties()->GetPacketTimeout();
    if (timeout_seconds > 0)
        m_gdb_comm.SetPacketTimeout(timeout_seconds);
}

//----------------------------------------------------------------------
// Destructor
//----------------------------------------------------------------------
ProcessGDBRemote::~ProcessGDBRemote()
{
    //  m_mach_process.UnregisterNotificationCallbacks (this);
    Clear();
    // We need to call finalize on the process before destroying ourselves
    // to make sure all of the broadcaster cleanup goes as planned. If we
    // destruct this class, then Process::~Process() might have problems
    // trying to fully destroy the broadcaster.
    Finalize();
    
    // The general Finalize is going to try to destroy the process and that SHOULD
    // shut down the async thread.  However, if we don't kill it it will get stranded and
    // its connection will go away so when it wakes up it will crash.  So kill it for sure here.
    StopAsyncThread();
    KillDebugserverProcess();
}

//----------------------------------------------------------------------
// PluginInterface
//----------------------------------------------------------------------
ConstString
ProcessGDBRemote::GetPluginName()
{
    return GetPluginNameStatic();
}

uint32_t
ProcessGDBRemote::GetPluginVersion()
{
    return 1;
}

bool
ProcessGDBRemote::ParsePythonTargetDefinition(const FileSpec &target_definition_fspec)
{
#ifndef LLDB_DISABLE_PYTHON
    ScriptInterpreter *interpreter = GetTarget().GetDebugger().GetCommandInterpreter().GetScriptInterpreter();
    Error error;
    lldb::ScriptInterpreterObjectSP module_object_sp (interpreter->LoadPluginModule(target_definition_fspec, error));
    if (module_object_sp)
    {
        lldb::ScriptInterpreterObjectSP target_definition_sp (interpreter->GetDynamicSettings(module_object_sp,
                                                                                              &GetTarget(),
                                                                                              "gdb-server-target-definition",
                                                                                              error));
        
        PythonDictionary target_dict(target_definition_sp);

        if (target_dict)
        {
            PythonDictionary host_info_dict (target_dict.GetItemForKey("host-info"));
            if (host_info_dict)
            {
                ArchSpec host_arch (host_info_dict.GetItemForKeyAsString(PythonString("triple")));
                
                if (!host_arch.IsCompatibleMatch(GetTarget().GetArchitecture()))
                {
                    GetTarget().SetArchitecture(host_arch);
                }
                    
            }
            m_breakpoint_pc_offset = target_dict.GetItemForKeyAsInteger("breakpoint-pc-offset", 0);

            if (m_register_info.SetRegisterInfo (target_dict, GetTarget().GetArchitecture().GetByteOrder()) > 0)
            {
                return true;
            }
        }
    }
#endif
    return false;
}


void
ProcessGDBRemote::BuildDynamicRegisterInfo (bool force)
{
    if (!force && m_register_info.GetNumRegisters() > 0)
        return;

    char packet[128];
    m_register_info.Clear();
    uint32_t reg_offset = 0;
    uint32_t reg_num = 0;
    for (StringExtractorGDBRemote::ResponseType response_type = StringExtractorGDBRemote::eResponse;
         response_type == StringExtractorGDBRemote::eResponse; 
         ++reg_num)
    {
        const int packet_len = ::snprintf (packet, sizeof(packet), "qRegisterInfo%x", reg_num);
        assert (packet_len < (int)sizeof(packet));
        StringExtractorGDBRemote response;
        if (m_gdb_comm.SendPacketAndWaitForResponse(packet, packet_len, response, false) == GDBRemoteCommunication::PacketResult::Success)
        {
            response_type = response.GetResponseType();
            if (response_type == StringExtractorGDBRemote::eResponse)
            {
                std::string name;
                std::string value;
                ConstString reg_name;
                ConstString alt_name;
                ConstString set_name;
                std::vector<uint32_t> value_regs;
                std::vector<uint32_t> invalidate_regs;
                RegisterInfo reg_info = { NULL,                 // Name
                    NULL,                 // Alt name
                    0,                    // byte size
                    reg_offset,           // offset
                    eEncodingUint,        // encoding
                    eFormatHex,           // formate
                    {
                        LLDB_INVALID_REGNUM, // GCC reg num
                        LLDB_INVALID_REGNUM, // DWARF reg num
                        LLDB_INVALID_REGNUM, // generic reg num
                        reg_num,             // GDB reg num
                        reg_num           // native register number
                    },
                    NULL,
                    NULL
                };

                while (response.GetNameColonValue(name, value))
                {
                    if (name.compare("name") == 0)
                    {
                        reg_name.SetCString(value.c_str());
                    }
                    else if (name.compare("alt-name") == 0)
                    {
                        alt_name.SetCString(value.c_str());
                    }
                    else if (name.compare("bitsize") == 0)
                    {
                        reg_info.byte_size = Args::StringToUInt32(value.c_str(), 0, 0) / CHAR_BIT;
                    }
                    else if (name.compare("offset") == 0)
                    {
                        uint32_t offset = Args::StringToUInt32(value.c_str(), UINT32_MAX, 0);
                        if (reg_offset != offset)
                        {
                            reg_offset = offset;
                        }
                    }
                    else if (name.compare("encoding") == 0)
                    {
                        const Encoding encoding = Args::StringToEncoding (value.c_str());
                        if (encoding != eEncodingInvalid)
                            reg_info.encoding = encoding;
                    }
                    else if (name.compare("format") == 0)
                    {
                        Format format = eFormatInvalid;
                        if (Args::StringToFormat (value.c_str(), format, NULL).Success())
                            reg_info.format = format;
                        else if (value.compare("binary") == 0)
                            reg_info.format = eFormatBinary;
                        else if (value.compare("decimal") == 0)
                            reg_info.format = eFormatDecimal;
                        else if (value.compare("hex") == 0)
                            reg_info.format = eFormatHex;
                        else if (value.compare("float") == 0)
                            reg_info.format = eFormatFloat;
                        else if (value.compare("vector-sint8") == 0)
                            reg_info.format = eFormatVectorOfSInt8;
                        else if (value.compare("vector-uint8") == 0)
                            reg_info.format = eFormatVectorOfUInt8;
                        else if (value.compare("vector-sint16") == 0)
                            reg_info.format = eFormatVectorOfSInt16;
                        else if (value.compare("vector-uint16") == 0)
                            reg_info.format = eFormatVectorOfUInt16;
                        else if (value.compare("vector-sint32") == 0)
                            reg_info.format = eFormatVectorOfSInt32;
                        else if (value.compare("vector-uint32") == 0)
                            reg_info.format = eFormatVectorOfUInt32;
                        else if (value.compare("vector-float32") == 0)
                            reg_info.format = eFormatVectorOfFloat32;
                        else if (value.compare("vector-uint128") == 0)
                            reg_info.format = eFormatVectorOfUInt128;
                    }
                    else if (name.compare("set") == 0)
                    {
                        set_name.SetCString(value.c_str());
                    }
                    else if (name.compare("gcc") == 0)
                    {
                        reg_info.kinds[eRegisterKindGCC] = Args::StringToUInt32(value.c_str(), LLDB_INVALID_REGNUM, 0);
                    }
                    else if (name.compare("dwarf") == 0)
                    {
                        reg_info.kinds[eRegisterKindDWARF] = Args::StringToUInt32(value.c_str(), LLDB_INVALID_REGNUM, 0);
                    }
                    else if (name.compare("generic") == 0)
                    {
                        reg_info.kinds[eRegisterKindGeneric] = Args::StringToGenericRegister (value.c_str());
                    }
                    else if (name.compare("container-regs") == 0)
                    {
                        std::pair<llvm::StringRef, llvm::StringRef> value_pair;
                        value_pair.second = value;
                        do
                        {
                            value_pair = value_pair.second.split(',');
                            if (!value_pair.first.empty())
                            {
                                uint32_t reg = Args::StringToUInt32 (value_pair.first.str().c_str(), LLDB_INVALID_REGNUM, 16);
                                if (reg != LLDB_INVALID_REGNUM)
                                    value_regs.push_back (reg);
                            }
                        } while (!value_pair.second.empty());
                    }
                    else if (name.compare("invalidate-regs") == 0)
                    {
                        std::pair<llvm::StringRef, llvm::StringRef> value_pair;
                        value_pair.second = value;
                        do
                        {
                            value_pair = value_pair.second.split(',');
                            if (!value_pair.first.empty())
                            {
                                uint32_t reg = Args::StringToUInt32 (value_pair.first.str().c_str(), LLDB_INVALID_REGNUM, 16);
                                if (reg != LLDB_INVALID_REGNUM)
                                    invalidate_regs.push_back (reg);
                            }
                        } while (!value_pair.second.empty());
                    }
                }

                reg_info.byte_offset = reg_offset;
                assert (reg_info.byte_size != 0);
                reg_offset += reg_info.byte_size;
                if (!value_regs.empty())
                {
                    value_regs.push_back(LLDB_INVALID_REGNUM);
                    reg_info.value_regs = value_regs.data();
                }
                if (!invalidate_regs.empty())
                {
                    invalidate_regs.push_back(LLDB_INVALID_REGNUM);
                    reg_info.invalidate_regs = invalidate_regs.data();
                }

                m_register_info.AddRegister(reg_info, reg_name, alt_name, set_name);
            }
            else
            {
                break;  // ensure exit before reg_num is incremented
            }
        }
        else
        {
            break;
        }
    }

    // Check if qHostInfo specified a specific packet timeout for this connection.
    // If so then lets update our setting so the user knows what the timeout is
    // and can see it.
    const uint32_t host_packet_timeout = m_gdb_comm.GetHostDefaultPacketTimeout();
    if (host_packet_timeout)
    {
        GetGlobalPluginProperties()->SetPacketTimeout(host_packet_timeout);
    }
    

    if (reg_num == 0)
    {
        FileSpec target_definition_fspec = GetGlobalPluginProperties()->GetTargetDefinitionFile ();
        
        if (target_definition_fspec)
        {
            // See if we can get register definitions from a python file
            if (ParsePythonTargetDefinition (target_definition_fspec))
                return;
        }
    }

    // We didn't get anything if the accumulated reg_num is zero.  See if we are
    // debugging ARM and fill with a hard coded register set until we can get an
    // updated debugserver down on the devices.
    // On the other hand, if the accumulated reg_num is positive, see if we can
    // add composite registers to the existing primordial ones.
    bool from_scratch = (reg_num == 0);

    const ArchSpec &target_arch = GetTarget().GetArchitecture();
    const ArchSpec &remote_host_arch = m_gdb_comm.GetHostArchitecture();
    const ArchSpec &remote_process_arch = m_gdb_comm.GetProcessArchitecture();

    // Use the process' architecture instead of the host arch, if available
    ArchSpec remote_arch;
    if (remote_process_arch.IsValid ())
        remote_arch = remote_process_arch;
    else
        remote_arch = remote_host_arch;

    if (!target_arch.IsValid())
    {
        if (remote_arch.IsValid()
              && remote_arch.GetMachine() == llvm::Triple::arm
              && remote_arch.GetTriple().getVendor() == llvm::Triple::Apple)
            m_register_info.HardcodeARMRegisters(from_scratch);
    }
    else if (target_arch.GetMachine() == llvm::Triple::arm)
    {
        m_register_info.HardcodeARMRegisters(from_scratch);
    }

    // At this point, we can finalize our register info.
    m_register_info.Finalize ();
}

Error
ProcessGDBRemote::WillLaunch (Module* module)
{
    return WillLaunchOrAttach ();
}

Error
ProcessGDBRemote::WillAttachToProcessWithID (lldb::pid_t pid)
{
    return WillLaunchOrAttach ();
}

Error
ProcessGDBRemote::WillAttachToProcessWithName (const char *process_name, bool wait_for_launch)
{
    return WillLaunchOrAttach ();
}

Error
ProcessGDBRemote::DoConnectRemote (Stream *strm, const char *remote_url)
{
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_PROCESS));
    Error error (WillLaunchOrAttach ());
    
    if (error.Fail())
        return error;

    error = ConnectToDebugserver (remote_url);

    if (error.Fail())
        return error;
    StartAsyncThread ();

    lldb::pid_t pid = m_gdb_comm.GetCurrentProcessID ();
    if (pid == LLDB_INVALID_PROCESS_ID)
    {
        // We don't have a valid process ID, so note that we are connected
        // and could now request to launch or attach, or get remote process 
        // listings...
        SetPrivateState (eStateConnected);
    }
    else
    {
        // We have a valid process
        SetID (pid);
        GetThreadList();
        if (m_gdb_comm.SendPacketAndWaitForResponse("?", 1, m_last_stop_packet, false) == GDBRemoteCommunication::PacketResult::Success)
        {
            if (!m_target.GetArchitecture().IsValid()) 
            {
                if (m_gdb_comm.GetProcessArchitecture().IsValid())
                {
                    m_target.SetArchitecture(m_gdb_comm.GetProcessArchitecture());
                }
                else
                {
                    m_target.SetArchitecture(m_gdb_comm.GetHostArchitecture());
                }
            }

            const StateType state = SetThreadStopInfo (m_last_stop_packet);
            if (state == eStateStopped)
            {
                SetPrivateState (state);
            }
            else
                error.SetErrorStringWithFormat ("Process %" PRIu64 " was reported after connecting to '%s', but state was not stopped: %s", pid, remote_url, StateAsCString (state));
        }
        else
            error.SetErrorStringWithFormat ("Process %" PRIu64 " was reported after connecting to '%s', but no stop reply packet was received", pid, remote_url);
    }

    if (log)
        log->Printf ("ProcessGDBRemote::%s pid %" PRIu64 ": normalizing target architecture initial triple: %s (GetTarget().GetArchitecture().IsValid() %s, m_gdb_comm.GetHostArchitecture().IsValid(): %s)", __FUNCTION__, GetID (), GetTarget ().GetArchitecture ().GetTriple ().getTriple ().c_str (), GetTarget ().GetArchitecture ().IsValid () ? "true" : "false", m_gdb_comm.GetHostArchitecture ().IsValid () ? "true" : "false");


    if (error.Success()
        && !GetTarget().GetArchitecture().IsValid()
        && m_gdb_comm.GetHostArchitecture().IsValid())
    {
        // Prefer the *process'* architecture over that of the *host*, if available.
        if (m_gdb_comm.GetProcessArchitecture().IsValid())
            GetTarget().SetArchitecture(m_gdb_comm.GetProcessArchitecture());
        else
            GetTarget().SetArchitecture(m_gdb_comm.GetHostArchitecture());
    }

    if (log)
        log->Printf ("ProcessGDBRemote::%s pid %" PRIu64 ": normalized target architecture triple: %s", __FUNCTION__, GetID (), GetTarget ().GetArchitecture ().GetTriple ().getTriple ().c_str ());

    // Set the Unix signals properly for the target.
    // FIXME Add a gdb-remote packet to discover dynamically.
    if (error.Success ())
    {
        const ArchSpec arch_spec = GetTarget ().GetArchitecture ();
        if (arch_spec.IsValid ())
        {
            if (log)
                log->Printf ("ProcessGDBRemote::%s pid %" PRIu64 ": determining unix signals type based on architecture %s, triple %s", __FUNCTION__, GetID (), arch_spec.GetArchitectureName () ? arch_spec.GetArchitectureName () : "<null>", arch_spec.GetTriple ().getTriple ().c_str ());

            switch (arch_spec.GetTriple ().getOS ())
            {
            case llvm::Triple::Linux:
                m_unix_signals_sp.reset (new process_linux::LinuxSignals ());
                if (log)
                    log->Printf ("ProcessGDBRemote::%s using Linux unix signals type for pid %" PRIu64, __FUNCTION__, GetID ());
                break;
            case llvm::Triple::OpenBSD:
            case llvm::Triple::FreeBSD:
            case llvm::Triple::NetBSD:
                m_unix_signals_sp.reset (new FreeBSDSignals ());
                if (log)
                    log->Printf ("ProcessGDBRemote::%s using *BSD unix signals type for pid %" PRIu64, __FUNCTION__, GetID ());
                break;
            default:
                m_unix_signals_sp.reset (new UnixSignals ());
                if (log)
                    log->Printf ("ProcessGDBRemote::%s using generic unix signals type for pid %" PRIu64, __FUNCTION__, GetID ());
                break;
            }
        }
    }

    return error;
}

Error
ProcessGDBRemote::WillLaunchOrAttach ()
{
    Error error;
    m_stdio_communication.Clear ();
    return error;
}

//----------------------------------------------------------------------
// Process Control
//----------------------------------------------------------------------
Error
ProcessGDBRemote::DoLaunch (Module *exe_module, ProcessLaunchInfo &launch_info)
{
    Error error;

    uint32_t launch_flags = launch_info.GetFlags().Get();
    const char *stdin_path = NULL;
    const char *stdout_path = NULL;
    const char *stderr_path = NULL;
    const char *working_dir = launch_info.GetWorkingDirectory();

    const ProcessLaunchInfo::FileAction *file_action;
    file_action = launch_info.GetFileActionForFD (STDIN_FILENO);
    if (file_action)
    {
        if (file_action->GetAction () == ProcessLaunchInfo::FileAction::eFileActionOpen)
            stdin_path = file_action->GetPath();
    }
    file_action = launch_info.GetFileActionForFD (STDOUT_FILENO);
    if (file_action)
    {
        if (file_action->GetAction () == ProcessLaunchInfo::FileAction::eFileActionOpen)
            stdout_path = file_action->GetPath();
    }
    file_action = launch_info.GetFileActionForFD (STDERR_FILENO);
    if (file_action)
    {
        if (file_action->GetAction () == ProcessLaunchInfo::FileAction::eFileActionOpen)
            stderr_path = file_action->GetPath();
    }

    //  ::LogSetBitMask (GDBR_LOG_DEFAULT);
    //  ::LogSetOptions (LLDB_LOG_OPTION_THREADSAFE | LLDB_LOG_OPTION_PREPEND_TIMESTAMP | LLDB_LOG_OPTION_PREPEND_PROC_AND_THREAD);
    //  ::LogSetLogFile ("/dev/stdout");
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_PROCESS));

    ObjectFile * object_file = exe_module->GetObjectFile();
    if (object_file)
    {
        // Make sure we aren't already connected?
        if (!m_gdb_comm.IsConnected())
        {
            error = LaunchAndConnectToDebugserver (launch_info);
        }
        
        if (error.Success())
        {
            lldb_utility::PseudoTerminal pty;
            const bool disable_stdio = (launch_flags & eLaunchFlagDisableSTDIO) != 0;

            // If the debugserver is local and we aren't disabling STDIO, lets use
            // a pseudo terminal to instead of relying on the 'O' packets for stdio
            // since 'O' packets can really slow down debugging if the inferior 
            // does a lot of output.
            PlatformSP platform_sp (m_target.GetPlatform());
            if (platform_sp && platform_sp->IsHost() && !disable_stdio)
            {
                const char *slave_name = NULL;
                if (stdin_path == NULL || stdout_path == NULL || stderr_path == NULL)
                {
                    if (pty.OpenFirstAvailableMaster(O_RDWR|O_NOCTTY, NULL, 0))
                        slave_name = pty.GetSlaveName (NULL, 0);
                }
                if (stdin_path == NULL) 
                    stdin_path = slave_name;

                if (stdout_path == NULL)
                    stdout_path = slave_name;

                if (stderr_path == NULL)
                    stderr_path = slave_name;
            }

            // Set STDIN to /dev/null if we want STDIO disabled or if either
            // STDOUT or STDERR have been set to something and STDIN hasn't
            if (disable_stdio || (stdin_path == NULL && (stdout_path || stderr_path)))
                stdin_path = "/dev/null";
            
            // Set STDOUT to /dev/null if we want STDIO disabled or if either
            // STDIN or STDERR have been set to something and STDOUT hasn't
            if (disable_stdio || (stdout_path == NULL && (stdin_path || stderr_path)))
                stdout_path = "/dev/null";
            
            // Set STDERR to /dev/null if we want STDIO disabled or if either
            // STDIN or STDOUT have been set to something and STDERR hasn't
            if (disable_stdio || (stderr_path == NULL && (stdin_path || stdout_path)))
                stderr_path = "/dev/null";

            if (stdin_path) 
                m_gdb_comm.SetSTDIN (stdin_path);
            if (stdout_path)
                m_gdb_comm.SetSTDOUT (stdout_path);
            if (stderr_path)
                m_gdb_comm.SetSTDERR (stderr_path);

            m_gdb_comm.SetDisableASLR (launch_flags & eLaunchFlagDisableASLR);
            m_gdb_comm.SetDetachOnError (launch_flags & eLaunchFlagDetachOnError);

            m_gdb_comm.SendLaunchArchPacket (m_target.GetArchitecture().GetArchitectureName());
            
            const char * launch_event_data = launch_info.GetLaunchEventData();
            if (launch_event_data != NULL && *launch_event_data != '\0')
                m_gdb_comm.SendLaunchEventDataPacket (launch_event_data);
            
            if (working_dir && working_dir[0])
            {
                m_gdb_comm.SetWorkingDir (working_dir);
            }

            // Send the environment and the program + arguments after we connect
            const Args &environment = launch_info.GetEnvironmentEntries();
            if (environment.GetArgumentCount())
            {
                size_t num_environment_entries = environment.GetArgumentCount();
                for (size_t i=0; i<num_environment_entries; ++i)
                {
                    const char *env_entry = environment.GetArgumentAtIndex(i);
                    if (env_entry == NULL || m_gdb_comm.SendEnvironmentPacket(env_entry) != 0)
                        break;
                }
            }

            const uint32_t old_packet_timeout = m_gdb_comm.SetPacketTimeout (10);
            int arg_packet_err = m_gdb_comm.SendArgumentsPacket (launch_info);
            if (arg_packet_err == 0)
            {
                std::string error_str;
                if (m_gdb_comm.GetLaunchSuccess (error_str))
                {
                    SetID (m_gdb_comm.GetCurrentProcessID ());
                }
                else
                {
                    error.SetErrorString (error_str.c_str());
                }
            }
            else
            {
                error.SetErrorStringWithFormat("'A' packet returned an error: %i", arg_packet_err);
            }
            
            m_gdb_comm.SetPacketTimeout (old_packet_timeout);
                
            if (GetID() == LLDB_INVALID_PROCESS_ID)
            {
                if (log)
                    log->Printf("failed to connect to debugserver: %s", error.AsCString());
                KillDebugserverProcess ();
                return error;
            }

            if (m_gdb_comm.SendPacketAndWaitForResponse("?", 1, m_last_stop_packet, false) == GDBRemoteCommunication::PacketResult::Success)
            {
                if (!m_target.GetArchitecture().IsValid()) 
                {
                    if (m_gdb_comm.GetProcessArchitecture().IsValid())
                    {
                        m_target.SetArchitecture(m_gdb_comm.GetProcessArchitecture());
                    }
                    else
                    {
                        m_target.SetArchitecture(m_gdb_comm.GetHostArchitecture());
                    }
                }

                SetPrivateState (SetThreadStopInfo (m_last_stop_packet));
                
                if (!disable_stdio)
                {
                    if (pty.GetMasterFileDescriptor() != lldb_utility::PseudoTerminal::invalid_fd)
                        SetSTDIOFileDescriptor (pty.ReleaseMasterFileDescriptor());
                }
            }
        }
        else
        {
            if (log)
                log->Printf("failed to connect to debugserver: %s", error.AsCString());
        }
    }
    else
    {
        // Set our user ID to an invalid process ID.
        SetID(LLDB_INVALID_PROCESS_ID);
        error.SetErrorStringWithFormat ("failed to get object file from '%s' for arch %s", 
                                        exe_module->GetFileSpec().GetFilename().AsCString(), 
                                        exe_module->GetArchitecture().GetArchitectureName());
    }
    return error;

}


Error
ProcessGDBRemote::ConnectToDebugserver (const char *connect_url)
{
    Error error;
    // Only connect if we have a valid connect URL
    
    if (connect_url && connect_url[0])
    {
        std::unique_ptr<ConnectionFileDescriptor> conn_ap(new ConnectionFileDescriptor());
        if (conn_ap.get())
        {
            const uint32_t max_retry_count = 50;
            uint32_t retry_count = 0;
            while (!m_gdb_comm.IsConnected())
            {
                if (conn_ap->Connect(connect_url, &error) == eConnectionStatusSuccess)
                {
                    m_gdb_comm.SetConnection (conn_ap.release());
                    break;
                }
                else if (error.WasInterrupted())
                {
                    // If we were interrupted, don't keep retrying.
                    break;
                }
                
                retry_count++;
                
                if (retry_count >= max_retry_count)
                    break;

                usleep (100000);
            }
        }
    }

    if (!m_gdb_comm.IsConnected())
    {
        if (error.Success())
            error.SetErrorString("not connected to remote gdb server");
        return error;
    }

    // We always seem to be able to open a connection to a local port
    // so we need to make sure we can then send data to it. If we can't
    // then we aren't actually connected to anything, so try and do the
    // handshake with the remote GDB server and make sure that goes 
    // alright.
    if (!m_gdb_comm.HandshakeWithServer (&error))
    {
        m_gdb_comm.Disconnect();
        if (error.Success())
            error.SetErrorString("not connected to remote gdb server");
        return error;
    }
    m_gdb_comm.GetThreadSuffixSupported ();
    m_gdb_comm.GetListThreadsInStopReplySupported ();
    m_gdb_comm.GetHostInfo ();
    m_gdb_comm.GetVContSupported ('c');
    m_gdb_comm.GetVAttachOrWaitSupported();
    
    size_t num_cmds = GetExtraStartupCommands().GetArgumentCount();
    for (size_t idx = 0; idx < num_cmds; idx++)
    {
        StringExtractorGDBRemote response;
        m_gdb_comm.SendPacketAndWaitForResponse (GetExtraStartupCommands().GetArgumentAtIndex(idx), response, false);
    }
    return error;
}

void
ProcessGDBRemote::DidLaunchOrAttach (ArchSpec& process_arch)
{
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_PROCESS));
    if (log)
        log->Printf ("ProcessGDBRemote::DidLaunch()");
    if (GetID() != LLDB_INVALID_PROCESS_ID)
    {
        BuildDynamicRegisterInfo (false);

        // See if the GDB server supports the qHostInfo information


        // See if the GDB server supports the qProcessInfo packet, if so
        // prefer that over the Host information as it will be more specific
        // to our process.

        if (m_gdb_comm.GetProcessArchitecture().IsValid())
            process_arch = m_gdb_comm.GetProcessArchitecture();
        else
            process_arch = m_gdb_comm.GetHostArchitecture();

        if (process_arch.IsValid())
        {
            ArchSpec &target_arch = GetTarget().GetArchitecture();

            if (target_arch.IsValid())
            {
                // If the remote host is ARM and we have apple as the vendor, then 
                // ARM executables and shared libraries can have mixed ARM architectures.
                // You can have an armv6 executable, and if the host is armv7, then the
                // system will load the best possible architecture for all shared libraries
                // it has, so we really need to take the remote host architecture as our
                // defacto architecture in this case.

                if (process_arch.GetMachine() == llvm::Triple::arm &&
                    process_arch.GetTriple().getVendor() == llvm::Triple::Apple)
                {
                    GetTarget().SetArchitecture (process_arch);
                }
                else
                {
                    // Fill in what is missing in the triple
                    const llvm::Triple &remote_triple = process_arch.GetTriple();
                    llvm::Triple &target_triple = target_arch.GetTriple();
                    if (target_triple.getVendorName().size() == 0)
                    {
                        target_triple.setVendor (remote_triple.getVendor());

                        if (target_triple.getOSName().size() == 0)
                        {
                            target_triple.setOS (remote_triple.getOS());

                            if (target_triple.getEnvironmentName().size() == 0)
                                target_triple.setEnvironment (remote_triple.getEnvironment());
                        }
                    }
                }
            }
            else
            {
                // The target doesn't have a valid architecture yet, set it from
                // the architecture we got from the remote GDB server
                GetTarget().SetArchitecture (process_arch);
            }
        }
    }
}

void
ProcessGDBRemote::DidLaunch ()
{
    ArchSpec process_arch;
    DidLaunchOrAttach (process_arch);
}

UnixSignals&
ProcessGDBRemote::GetUnixSignals ()
{
    assert (m_unix_signals_sp && "m_unix_signals_sp is null");
    return *m_unix_signals_sp;
}

Error
ProcessGDBRemote::DoAttachToProcessWithID (lldb::pid_t attach_pid)
{
    ProcessAttachInfo attach_info;
    return DoAttachToProcessWithID(attach_pid, attach_info);
}

Error
ProcessGDBRemote::DoAttachToProcessWithID (lldb::pid_t attach_pid, const ProcessAttachInfo &attach_info)
{
    Error error;
    // Clear out and clean up from any current state
    Clear();
    if (attach_pid != LLDB_INVALID_PROCESS_ID)
    {
        // Make sure we aren't already connected?
        if (!m_gdb_comm.IsConnected())
        {
            error = LaunchAndConnectToDebugserver (attach_info);
            
            if (error.Fail())
            {
                const char *error_string = error.AsCString();
                if (error_string == NULL)
                    error_string = "unable to launch " DEBUGSERVER_BASENAME;

                SetExitStatus (-1, error_string);
            }
        }
    
        if (error.Success())
        {
            m_gdb_comm.SetDetachOnError(attach_info.GetDetachOnError());
            
            char packet[64];
            const int packet_len = ::snprintf (packet, sizeof(packet), "vAttach;%" PRIx64, attach_pid);
            SetID (attach_pid);            
            m_async_broadcaster.BroadcastEvent (eBroadcastBitAsyncContinue, new EventDataBytes (packet, packet_len));
        }
    }
    return error;
}

Error
ProcessGDBRemote::DoAttachToProcessWithName (const char *process_name, const ProcessAttachInfo &attach_info)
{
    Error error;
    // Clear out and clean up from any current state
    Clear();

    if (process_name && process_name[0])
    {
        // Make sure we aren't already connected?
        if (!m_gdb_comm.IsConnected())
        {
            error = LaunchAndConnectToDebugserver (attach_info);

            if (error.Fail())
            {
                const char *error_string = error.AsCString();
                if (error_string == NULL)
                    error_string = "unable to launch " DEBUGSERVER_BASENAME;

                SetExitStatus (-1, error_string);
            }
        }

        if (error.Success())
        {
            StreamString packet;
            
            m_gdb_comm.SetDetachOnError(attach_info.GetDetachOnError());
            
            if (attach_info.GetWaitForLaunch())
            {
                if (!m_gdb_comm.GetVAttachOrWaitSupported())
                {
                    packet.PutCString ("vAttachWait");
                }
                else
                {
                    if (attach_info.GetIgnoreExisting())
                        packet.PutCString("vAttachWait");
                    else
                        packet.PutCString ("vAttachOrWait");
                }
            }
            else
                packet.PutCString("vAttachName");
            packet.PutChar(';');
            packet.PutBytesAsRawHex8(process_name, strlen(process_name), lldb::endian::InlHostByteOrder(), lldb::endian::InlHostByteOrder());
            
            m_async_broadcaster.BroadcastEvent (eBroadcastBitAsyncContinue, new EventDataBytes (packet.GetData(), packet.GetSize()));

        }
    }
    return error;
}


bool
ProcessGDBRemote::SetExitStatus (int exit_status, const char *cstr)
{
    m_gdb_comm.Disconnect();
    return Process::SetExitStatus (exit_status, cstr);
}

void
ProcessGDBRemote::DidAttach (ArchSpec &process_arch)
{
    // If you can figure out what the architecture is, fill it in here.
    process_arch.Clear();
    DidLaunchOrAttach (process_arch);
}


Error
ProcessGDBRemote::WillResume ()
{
    m_continue_c_tids.clear();
    m_continue_C_tids.clear();
    m_continue_s_tids.clear();
    m_continue_S_tids.clear();
    return Error();
}

Error
ProcessGDBRemote::DoResume ()
{
    Error error;
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_PROCESS));
    if (log)
        log->Printf ("ProcessGDBRemote::Resume()");
    
    Listener listener ("gdb-remote.resume-packet-sent");
    if (listener.StartListeningForEvents (&m_gdb_comm, GDBRemoteCommunication::eBroadcastBitRunPacketSent))
    {
        listener.StartListeningForEvents (&m_async_broadcaster, ProcessGDBRemote::eBroadcastBitAsyncThreadDidExit);
        
        const size_t num_threads = GetThreadList().GetSize();

        StreamString continue_packet;
        bool continue_packet_error = false;
        if (m_gdb_comm.HasAnyVContSupport ())
        {
            if (m_continue_c_tids.size() == num_threads ||
                (m_continue_c_tids.empty() &&
                 m_continue_C_tids.empty() &&
                 m_continue_s_tids.empty() &&
                 m_continue_S_tids.empty()))
            {
                // All threads are continuing, just send a "c" packet
                continue_packet.PutCString ("c");
            }
            else
            {
                continue_packet.PutCString ("vCont");
            
                if (!m_continue_c_tids.empty())
                {
                    if (m_gdb_comm.GetVContSupported ('c'))
                    {
                        for (tid_collection::const_iterator t_pos = m_continue_c_tids.begin(), t_end = m_continue_c_tids.end(); t_pos != t_end; ++t_pos)
                            continue_packet.Printf(";c:%4.4" PRIx64, *t_pos);
                    }
                    else 
                        continue_packet_error = true;
                }
                
                if (!continue_packet_error && !m_continue_C_tids.empty())
                {
                    if (m_gdb_comm.GetVContSupported ('C'))
                    {
                        for (tid_sig_collection::const_iterator s_pos = m_continue_C_tids.begin(), s_end = m_continue_C_tids.end(); s_pos != s_end; ++s_pos)
                            continue_packet.Printf(";C%2.2x:%4.4" PRIx64, s_pos->second, s_pos->first);
                    }
                    else 
                        continue_packet_error = true;
                }

                if (!continue_packet_error && !m_continue_s_tids.empty())
                {
                    if (m_gdb_comm.GetVContSupported ('s'))
                    {
                        for (tid_collection::const_iterator t_pos = m_continue_s_tids.begin(), t_end = m_continue_s_tids.end(); t_pos != t_end; ++t_pos)
                            continue_packet.Printf(";s:%4.4" PRIx64, *t_pos);
                    }
                    else 
                        continue_packet_error = true;
                }
                
                if (!continue_packet_error && !m_continue_S_tids.empty())
                {
                    if (m_gdb_comm.GetVContSupported ('S'))
                    {
                        for (tid_sig_collection::const_iterator s_pos = m_continue_S_tids.begin(), s_end = m_continue_S_tids.end(); s_pos != s_end; ++s_pos)
                            continue_packet.Printf(";S%2.2x:%4.4" PRIx64, s_pos->second, s_pos->first);
                    }
                    else
                        continue_packet_error = true;
                }
                
                if (continue_packet_error)
                    continue_packet.GetString().clear();
            }
        }
        else
            continue_packet_error = true;
        
        if (continue_packet_error)
        {
            // Either no vCont support, or we tried to use part of the vCont
            // packet that wasn't supported by the remote GDB server.
            // We need to try and make a simple packet that can do our continue
            const size_t num_continue_c_tids = m_continue_c_tids.size();
            const size_t num_continue_C_tids = m_continue_C_tids.size();
            const size_t num_continue_s_tids = m_continue_s_tids.size();
            const size_t num_continue_S_tids = m_continue_S_tids.size();
            if (num_continue_c_tids > 0)
            {
                if (num_continue_c_tids == num_threads)
                {
                    // All threads are resuming...
                    m_gdb_comm.SetCurrentThreadForRun (-1);
                    continue_packet.PutChar ('c'); 
                    continue_packet_error = false;
                }
                else if (num_continue_c_tids == 1 &&
                         num_continue_C_tids == 0 && 
                         num_continue_s_tids == 0 && 
                         num_continue_S_tids == 0 )
                {
                    // Only one thread is continuing
                    m_gdb_comm.SetCurrentThreadForRun (m_continue_c_tids.front());
                    continue_packet.PutChar ('c');                
                    continue_packet_error = false;
                }
            }

            if (continue_packet_error && num_continue_C_tids > 0)
            {
                if ((num_continue_C_tids + num_continue_c_tids) == num_threads && 
                    num_continue_C_tids > 0 && 
                    num_continue_s_tids == 0 && 
                    num_continue_S_tids == 0 )
                {
                    const int continue_signo = m_continue_C_tids.front().second;
                    // Only one thread is continuing
                    if (num_continue_C_tids > 1)
                    {
                        // More that one thread with a signal, yet we don't have 
                        // vCont support and we are being asked to resume each
                        // thread with a signal, we need to make sure they are
                        // all the same signal, or we can't issue the continue
                        // accurately with the current support...
                        if (num_continue_C_tids > 1)
                        {
                            continue_packet_error = false;
                            for (size_t i=1; i<m_continue_C_tids.size(); ++i)
                            {
                                if (m_continue_C_tids[i].second != continue_signo)
                                    continue_packet_error = true;
                            }
                        }
                        if (!continue_packet_error)
                            m_gdb_comm.SetCurrentThreadForRun (-1);
                    }
                    else
                    {
                        // Set the continue thread ID
                        continue_packet_error = false;
                        m_gdb_comm.SetCurrentThreadForRun (m_continue_C_tids.front().first);
                    }
                    if (!continue_packet_error)
                    {
                        // Add threads continuing with the same signo...
                        continue_packet.Printf("C%2.2x", continue_signo);
                    }
                }
            }

            if (continue_packet_error && num_continue_s_tids > 0)
            {
                if (num_continue_s_tids == num_threads)
                {
                    // All threads are resuming...
                    m_gdb_comm.SetCurrentThreadForRun (-1);
                    continue_packet.PutChar ('s');
                    continue_packet_error = false;
                }
                else if (num_continue_c_tids == 0 &&
                         num_continue_C_tids == 0 && 
                         num_continue_s_tids == 1 && 
                         num_continue_S_tids == 0 )
                {
                    // Only one thread is stepping
                    m_gdb_comm.SetCurrentThreadForRun (m_continue_s_tids.front());
                    continue_packet.PutChar ('s');                
                    continue_packet_error = false;
                }
            }

            if (!continue_packet_error && num_continue_S_tids > 0)
            {
                if (num_continue_S_tids == num_threads)
                {
                    const int step_signo = m_continue_S_tids.front().second;
                    // Are all threads trying to step with the same signal?
                    continue_packet_error = false;
                    if (num_continue_S_tids > 1)
                    {
                        for (size_t i=1; i<num_threads; ++i)
                        {
                            if (m_continue_S_tids[i].second != step_signo)
                                continue_packet_error = true;
                        }
                    }
                    if (!continue_packet_error)
                    {
                        // Add threads stepping with the same signo...
                        m_gdb_comm.SetCurrentThreadForRun (-1);
                        continue_packet.Printf("S%2.2x", step_signo);
                    }
                }
                else if (num_continue_c_tids == 0 &&
                         num_continue_C_tids == 0 && 
                         num_continue_s_tids == 0 && 
                         num_continue_S_tids == 1 )
                {
                    // Only one thread is stepping with signal
                    m_gdb_comm.SetCurrentThreadForRun (m_continue_S_tids.front().first);
                    continue_packet.Printf("S%2.2x", m_continue_S_tids.front().second);
                    continue_packet_error = false;
                }
            }
        }

        if (continue_packet_error)
        {
            error.SetErrorString ("can't make continue packet for this resume");
        }
        else
        {
            EventSP event_sp;
            TimeValue timeout;
            timeout = TimeValue::Now();
            timeout.OffsetWithSeconds (5);
            if (!IS_VALID_LLDB_HOST_THREAD(m_async_thread))
            {
                error.SetErrorString ("Trying to resume but the async thread is dead.");
                if (log)
                    log->Printf ("ProcessGDBRemote::DoResume: Trying to resume but the async thread is dead.");
                return error;
            }
            
            m_async_broadcaster.BroadcastEvent (eBroadcastBitAsyncContinue, new EventDataBytes (continue_packet.GetData(), continue_packet.GetSize()));

            if (listener.WaitForEvent (&timeout, event_sp) == false)
            {
                error.SetErrorString("Resume timed out.");
                if (log)
                    log->Printf ("ProcessGDBRemote::DoResume: Resume timed out.");
            }
            else if (event_sp->BroadcasterIs (&m_async_broadcaster))
            {
                error.SetErrorString ("Broadcast continue, but the async thread was killed before we got an ack back.");
                if (log)
                    log->Printf ("ProcessGDBRemote::DoResume: Broadcast continue, but the async thread was killed before we got an ack back.");
                return error;
            }
        }
    }

    return error;
}

void
ProcessGDBRemote::ClearThreadIDList ()
{
    Mutex::Locker locker(m_thread_list_real.GetMutex());
    m_thread_ids.clear();
}

bool
ProcessGDBRemote::UpdateThreadIDList ()
{
    Mutex::Locker locker(m_thread_list_real.GetMutex());
    bool sequence_mutex_unavailable = false;
    m_gdb_comm.GetCurrentThreadIDs (m_thread_ids, sequence_mutex_unavailable);
    if (sequence_mutex_unavailable)
    {
        return false; // We just didn't get the list
    }
    return true;
}

bool
ProcessGDBRemote::UpdateThreadList (ThreadList &old_thread_list, ThreadList &new_thread_list)
{
    // locker will keep a mutex locked until it goes out of scope
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_THREAD));
    if (log && log->GetMask().Test(GDBR_LOG_VERBOSE))
        log->Printf ("ProcessGDBRemote::%s (pid = %" PRIu64 ")", __FUNCTION__, GetID());
    
    size_t num_thread_ids = m_thread_ids.size();
    // The "m_thread_ids" thread ID list should always be updated after each stop
    // reply packet, but in case it isn't, update it here.
    if (num_thread_ids == 0)
    {
        if (!UpdateThreadIDList ())
            return false;
        num_thread_ids = m_thread_ids.size();
    }

    ThreadList old_thread_list_copy(old_thread_list);
    if (num_thread_ids > 0)
    {
        for (size_t i=0; i<num_thread_ids; ++i)
        {
            tid_t tid = m_thread_ids[i];
            ThreadSP thread_sp (old_thread_list_copy.RemoveThreadByProtocolID(tid, false));
            if (!thread_sp)
            {
                thread_sp.reset (new ThreadGDBRemote (*this, tid));
                if (log && log->GetMask().Test(GDBR_LOG_VERBOSE))
                    log->Printf(
                            "ProcessGDBRemote::%s Making new thread: %p for thread ID: 0x%" PRIx64 ".\n",
                            __FUNCTION__, static_cast<void*>(thread_sp.get()),
                            thread_sp->GetID());
            }
            else
            {
                if (log && log->GetMask().Test(GDBR_LOG_VERBOSE))
                    log->Printf(
                           "ProcessGDBRemote::%s Found old thread: %p for thread ID: 0x%" PRIx64 ".\n",
                           __FUNCTION__, static_cast<void*>(thread_sp.get()),
                           thread_sp->GetID());
            }
            new_thread_list.AddThread(thread_sp);
        }
    }
    
    // Whatever that is left in old_thread_list_copy are not
    // present in new_thread_list. Remove non-existent threads from internal id table.
    size_t old_num_thread_ids = old_thread_list_copy.GetSize(false);
    for (size_t i=0; i<old_num_thread_ids; i++)
    {
        ThreadSP old_thread_sp(old_thread_list_copy.GetThreadAtIndex (i, false));
        if (old_thread_sp)
        {
            lldb::tid_t old_thread_id = old_thread_sp->GetProtocolID();
            m_thread_id_to_index_id_map.erase(old_thread_id);
        }
    }
    
    return true;
}


StateType
ProcessGDBRemote::SetThreadStopInfo (StringExtractor& stop_packet)
{
    stop_packet.SetFilePos (0);
    const char stop_type = stop_packet.GetChar();
    switch (stop_type)
    {
    case 'T':
    case 'S':
        {
            // This is a bit of a hack, but is is required. If we did exec, we
            // need to clear our thread lists and also know to rebuild our dynamic
            // register info before we lookup and threads and populate the expedited
            // register values so we need to know this right away so we can cleanup
            // and update our registers.
            const uint32_t stop_id = GetStopID();
            if (stop_id == 0)
            {
                // Our first stop, make sure we have a process ID, and also make
                // sure we know about our registers
                if (GetID() == LLDB_INVALID_PROCESS_ID)
                {
                    lldb::pid_t pid = m_gdb_comm.GetCurrentProcessID ();
                    if (pid != LLDB_INVALID_PROCESS_ID)
                        SetID (pid);
                }
                BuildDynamicRegisterInfo (true);
            }
            // Stop with signal and thread info
            const uint8_t signo = stop_packet.GetHexU8();
            std::string name;
            std::string value;
            std::string thread_name;
            std::string reason;
            std::string description;
            uint32_t exc_type = 0;
            std::vector<addr_t> exc_data;
            addr_t thread_dispatch_qaddr = LLDB_INVALID_ADDRESS;
            ThreadSP thread_sp;
            ThreadGDBRemote *gdb_thread = NULL;

            while (stop_packet.GetNameColonValue(name, value))
            {
                if (name.compare("metype") == 0)
                {
                    // exception type in big endian hex
                    exc_type = Args::StringToUInt32 (value.c_str(), 0, 16);
                }
                else if (name.compare("medata") == 0)
                {
                    // exception data in big endian hex
                    exc_data.push_back(Args::StringToUInt64 (value.c_str(), 0, 16));
                }
                else if (name.compare("thread") == 0)
                {
                    // thread in big endian hex
                    lldb::tid_t tid = Args::StringToUInt64 (value.c_str(), LLDB_INVALID_THREAD_ID, 16);
                    // m_thread_list_real does have its own mutex, but we need to
                    // hold onto the mutex between the call to m_thread_list_real.FindThreadByID(...)
                    // and the m_thread_list_real.AddThread(...) so it doesn't change on us
                    Mutex::Locker locker (m_thread_list_real.GetMutex ());
                    thread_sp = m_thread_list_real.FindThreadByProtocolID(tid, false);

                    if (!thread_sp)
                    {
                        // Create the thread if we need to
                        thread_sp.reset (new ThreadGDBRemote (*this, tid));
                        Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_THREAD));
                        if (log && log->GetMask().Test(GDBR_LOG_VERBOSE))
                            log->Printf ("ProcessGDBRemote::%s Adding new thread: %p for thread ID: 0x%" PRIx64 ".\n",
                                         __FUNCTION__,
                                         static_cast<void*>(thread_sp.get()),
                                         thread_sp->GetID());

                        m_thread_list_real.AddThread(thread_sp);
                    }
                    gdb_thread = static_cast<ThreadGDBRemote *> (thread_sp.get());

                }
                else if (name.compare("threads") == 0)
                {
                    Mutex::Locker locker(m_thread_list_real.GetMutex());
                    m_thread_ids.clear();
                    // A comma separated list of all threads in the current
                    // process that includes the thread for this stop reply
                    // packet
                    size_t comma_pos;
                    lldb::tid_t tid;
                    while ((comma_pos = value.find(',')) != std::string::npos)
                    {
                        value[comma_pos] = '\0';
                        // thread in big endian hex
                        tid = Args::StringToUInt64 (value.c_str(), LLDB_INVALID_THREAD_ID, 16);
                        if (tid != LLDB_INVALID_THREAD_ID)
                            m_thread_ids.push_back (tid);
                        value.erase(0, comma_pos + 1);
                    }
                    tid = Args::StringToUInt64 (value.c_str(), LLDB_INVALID_THREAD_ID, 16);
                    if (tid != LLDB_INVALID_THREAD_ID)
                        m_thread_ids.push_back (tid);
                }
                else if (name.compare("hexname") == 0)
                {
                    StringExtractor name_extractor;
                    // Swap "value" over into "name_extractor"
                    name_extractor.GetStringRef().swap(value);
                    // Now convert the HEX bytes into a string value
                    name_extractor.GetHexByteString (value);
                    thread_name.swap (value);
                }
                else if (name.compare("name") == 0)
                {
                    thread_name.swap (value);
                }
                else if (name.compare("qaddr") == 0)
                {
                    thread_dispatch_qaddr = Args::StringToUInt64 (value.c_str(), 0, 16);
                }
                else if (name.compare("reason") == 0)
                {
                    reason.swap(value);
                }
                else if (name.compare("description") == 0)
                {
                    StringExtractor desc_extractor;
                    // Swap "value" over into "name_extractor"
                    desc_extractor.GetStringRef().swap(value);
                    // Now convert the HEX bytes into a string value
                    desc_extractor.GetHexByteString (thread_name);
                }
                else if (name.size() == 2 && ::isxdigit(name[0]) && ::isxdigit(name[1]))
                {
                    // We have a register number that contains an expedited
                    // register value. Lets supply this register to our thread
                    // so it won't have to go and read it.
                    if (gdb_thread)
                    {
                        uint32_t reg = Args::StringToUInt32 (name.c_str(), UINT32_MAX, 16);

                        if (reg != UINT32_MAX)
                        {
                            StringExtractor reg_value_extractor;
                            // Swap "value" over into "reg_value_extractor"
                            reg_value_extractor.GetStringRef().swap(value);
                            if (!gdb_thread->PrivateSetRegisterValue (reg, reg_value_extractor))
                            {
                                Host::SetCrashDescriptionWithFormat("Setting thread register '%s' (decoded to %u (0x%x)) with value '%s' for stop packet: '%s'", 
                                                                    name.c_str(), 
                                                                    reg, 
                                                                    reg, 
                                                                    reg_value_extractor.GetStringRef().c_str(), 
                                                                    stop_packet.GetStringRef().c_str());
                            }
                        }
                    }
                }
            }

            // If the response is old style 'S' packet which does not provide us with thread information
            // then update the thread list and choose the first one.
            if (!thread_sp)
            {
                UpdateThreadIDList ();

                if (!m_thread_ids.empty ())
                {
                    Mutex::Locker locker (m_thread_list_real.GetMutex ());
                    thread_sp = m_thread_list_real.FindThreadByProtocolID (m_thread_ids.front (), false);
                    if (thread_sp)
                        gdb_thread = static_cast<ThreadGDBRemote *> (thread_sp.get ());
                }
            }

            if (thread_sp)
            {
                // Clear the stop info just in case we don't set it to anything
                thread_sp->SetStopInfo (StopInfoSP());

                gdb_thread->SetThreadDispatchQAddr (thread_dispatch_qaddr);
                gdb_thread->SetName (thread_name.empty() ? NULL : thread_name.c_str());
                if (exc_type != 0)
                {
                    const size_t exc_data_size = exc_data.size();

                    thread_sp->SetStopInfo (StopInfoMachException::CreateStopReasonWithMachException (*thread_sp,
                                                                                                      exc_type,
                                                                                                      exc_data_size,
                                                                                                      exc_data_size >= 1 ? exc_data[0] : 0,
                                                                                                      exc_data_size >= 2 ? exc_data[1] : 0,
                                                                                                      exc_data_size >= 3 ? exc_data[2] : 0));
                }
                else
                {
                    bool handled = false;
                    bool did_exec = false;
                    if (!reason.empty())
                    {
                        if (reason.compare("trace") == 0)
                        {
                            thread_sp->SetStopInfo (StopInfo::CreateStopReasonToTrace (*thread_sp));
                            handled = true;
                        }
                        else if (reason.compare("breakpoint") == 0)
                        {
                            addr_t pc = thread_sp->GetRegisterContext()->GetPC();
                            lldb::BreakpointSiteSP bp_site_sp = thread_sp->GetProcess()->GetBreakpointSiteList().FindByAddress(pc);
                            if (bp_site_sp)
                            {
                                // If the breakpoint is for this thread, then we'll report the hit, but if it is for another thread,
                                // we can just report no reason.  We don't need to worry about stepping over the breakpoint here, that
                                // will be taken care of when the thread resumes and notices that there's a breakpoint under the pc.
                                handled = true;
                                if (bp_site_sp->ValidForThisThread (thread_sp.get()))
                                {
                                    thread_sp->SetStopInfo (StopInfo::CreateStopReasonWithBreakpointSiteID (*thread_sp, bp_site_sp->GetID()));
                                }
                                else
                                {
                                    StopInfoSP invalid_stop_info_sp;
                                    thread_sp->SetStopInfo (invalid_stop_info_sp);
                                }
                            }
                        }
                        else if (reason.compare("trap") == 0)
                        {
                            // Let the trap just use the standard signal stop reason below...
                        }
                        else if (reason.compare("watchpoint") == 0)
                        {
                            break_id_t watch_id = LLDB_INVALID_WATCH_ID;
                            // TODO: locate the watchpoint somehow...
                            thread_sp->SetStopInfo (StopInfo::CreateStopReasonWithWatchpointID (*thread_sp, watch_id));
                            handled = true;
                        }
                        else if (reason.compare("exception") == 0)
                        {
                            thread_sp->SetStopInfo (StopInfo::CreateStopReasonWithException(*thread_sp, description.c_str()));
                            handled = true;
                        }
                        else if (reason.compare("exec") == 0)
                        {
                            did_exec = true;
                            thread_sp->SetStopInfo (StopInfo::CreateStopReasonWithExec(*thread_sp));
                            handled = true;
                        }
                    }

                    if (!handled && signo && did_exec == false)
                    {
                        if (signo == SIGTRAP)
                        {
                            // Currently we are going to assume SIGTRAP means we are either
                            // hitting a breakpoint or hardware single stepping. 
                            handled = true;
                            addr_t pc = thread_sp->GetRegisterContext()->GetPC() + m_breakpoint_pc_offset;
                            lldb::BreakpointSiteSP bp_site_sp = thread_sp->GetProcess()->GetBreakpointSiteList().FindByAddress(pc);

                            if (bp_site_sp)
                            {
                                // If the breakpoint is for this thread, then we'll report the hit, but if it is for another thread,
                                // we can just report no reason.  We don't need to worry about stepping over the breakpoint here, that
                                // will be taken care of when the thread resumes and notices that there's a breakpoint under the pc.
                                if (bp_site_sp->ValidForThisThread (thread_sp.get()))
                                {
                                    if(m_breakpoint_pc_offset != 0)
                                        thread_sp->GetRegisterContext()->SetPC(pc);
                                    thread_sp->SetStopInfo (StopInfo::CreateStopReasonWithBreakpointSiteID (*thread_sp, bp_site_sp->GetID()));
                                }
                                else
                                {
                                    StopInfoSP invalid_stop_info_sp;
                                    thread_sp->SetStopInfo (invalid_stop_info_sp);
                                }
                            }
                            else
                            {
                                // If we were stepping then assume the stop was the result of the trace.  If we were
                                // not stepping then report the SIGTRAP.
                                // FIXME: We are still missing the case where we single step over a trap instruction.
                                if (thread_sp->GetTemporaryResumeState() == eStateStepping)
                                    thread_sp->SetStopInfo (StopInfo::CreateStopReasonToTrace (*thread_sp));
                                else
                                    thread_sp->SetStopInfo (StopInfo::CreateStopReasonWithSignal(*thread_sp, signo));
                            }
                        }
                        if (!handled)
                            thread_sp->SetStopInfo (StopInfo::CreateStopReasonWithSignal (*thread_sp, signo));
                    }

                    if (!description.empty())
                    {
                        lldb::StopInfoSP stop_info_sp (thread_sp->GetStopInfo ());
                        if (stop_info_sp)
                        {
                            stop_info_sp->SetDescription (description.c_str());
                        }
                        else
                        {
                            thread_sp->SetStopInfo (StopInfo::CreateStopReasonWithException (*thread_sp, description.c_str()));
                        }
                    }
                }
            }
            return eStateStopped;
        }
        break;

    case 'W':
    case 'X':
        // process exited
        return eStateExited;

    default:
        break;
    }
    return eStateInvalid;
}

void
ProcessGDBRemote::RefreshStateAfterStop ()
{
    Mutex::Locker locker(m_thread_list_real.GetMutex());
    m_thread_ids.clear();
    // Set the thread stop info. It might have a "threads" key whose value is
    // a list of all thread IDs in the current process, so m_thread_ids might
    // get set.
    SetThreadStopInfo (m_last_stop_packet);
    // Check to see if SetThreadStopInfo() filled in m_thread_ids?
    if (m_thread_ids.empty())
    {
        // No, we need to fetch the thread list manually
        UpdateThreadIDList();
    }

    // Let all threads recover from stopping and do any clean up based
    // on the previous thread state (if any).
    m_thread_list_real.RefreshStateAfterStop();
    
}

Error
ProcessGDBRemote::DoHalt (bool &caused_stop)
{
    Error error;

    bool timed_out = false;
    Mutex::Locker locker;
    
    if (m_public_state.GetValue() == eStateAttaching)
    {
        // We are being asked to halt during an attach. We need to just close
        // our file handle and debugserver will go away, and we can be done...
        m_gdb_comm.Disconnect();
    }
    else
    {
        if (!m_gdb_comm.SendInterrupt (locker, 2, timed_out))
        {
            if (timed_out)
                error.SetErrorString("timed out sending interrupt packet");
            else
                error.SetErrorString("unknown error sending interrupt packet");
        }
        
        caused_stop = m_gdb_comm.GetInterruptWasSent ();
    }
    return error;
}

Error
ProcessGDBRemote::DoDetach(bool keep_stopped)
{
    Error error;
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_PROCESS));
    if (log)
        log->Printf ("ProcessGDBRemote::DoDetach(keep_stopped: %i)", keep_stopped);
 
    error = m_gdb_comm.Detach (keep_stopped);
    if (log)
    {
        if (error.Success())
            log->PutCString ("ProcessGDBRemote::DoDetach() detach packet sent successfully");
        else
            log->Printf ("ProcessGDBRemote::DoDetach() detach packet send failed: %s", error.AsCString() ? error.AsCString() : "<unknown error>");
    }
    
    if (!error.Success())
        return error;

    // Sleep for one second to let the process get all detached...
    StopAsyncThread ();

    SetPrivateState (eStateDetached);
    ResumePrivateStateThread();

    //KillDebugserverProcess ();
    return error;
}


Error
ProcessGDBRemote::DoDestroy ()
{
    Error error;
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_PROCESS));
    if (log)
        log->Printf ("ProcessGDBRemote::DoDestroy()");

    // There is a bug in older iOS debugservers where they don't shut down the process
    // they are debugging properly.  If the process is sitting at a breakpoint or an exception,
    // this can cause problems with restarting.  So we check to see if any of our threads are stopped
    // at a breakpoint, and if so we remove all the breakpoints, resume the process, and THEN
    // destroy it again.
    //
    // Note, we don't have a good way to test the version of debugserver, but I happen to know that
    // the set of all the iOS debugservers which don't support GetThreadSuffixSupported() and that of
    // the debugservers with this bug are equal.  There really should be a better way to test this!
    //
    // We also use m_destroy_tried_resuming to make sure we only do this once, if we resume and then halt and
    // get called here to destroy again and we're still at a breakpoint or exception, then we should
    // just do the straight-forward kill.
    //
    // And of course, if we weren't able to stop the process by the time we get here, it isn't
    // necessary (or helpful) to do any of this.

    if (!m_gdb_comm.GetThreadSuffixSupported() && m_public_state.GetValue() != eStateRunning)
    {
        PlatformSP platform_sp = GetTarget().GetPlatform();
        
        // FIXME: These should be ConstStrings so we aren't doing strcmp'ing.
        if (platform_sp
            && platform_sp->GetName()
            && platform_sp->GetName() == PlatformRemoteiOS::GetPluginNameStatic())
        {
            if (m_destroy_tried_resuming)
            {
                if (log)
                    log->PutCString ("ProcessGDBRemote::DoDestroy() - Tried resuming to destroy once already, not doing it again.");
            }
            else
            {            
                // At present, the plans are discarded and the breakpoints disabled Process::Destroy,
                // but we really need it to happen here and it doesn't matter if we do it twice.
                m_thread_list.DiscardThreadPlans();
                DisableAllBreakpointSites();
                
                bool stop_looks_like_crash = false;
                ThreadList &threads = GetThreadList();
                
                {
                    Mutex::Locker locker(threads.GetMutex());
                    
                    size_t num_threads = threads.GetSize();
                    for (size_t i = 0; i < num_threads; i++)
                    {
                        ThreadSP thread_sp = threads.GetThreadAtIndex(i);
                        StopInfoSP stop_info_sp = thread_sp->GetPrivateStopInfo();
                        StopReason reason = eStopReasonInvalid;
                        if (stop_info_sp)
                            reason = stop_info_sp->GetStopReason();
                        if (reason == eStopReasonBreakpoint
                            || reason == eStopReasonException)
                        {
                            if (log)
                                log->Printf ("ProcessGDBRemote::DoDestroy() - thread: 0x%4.4" PRIx64 " stopped with reason: %s.",
                                             thread_sp->GetProtocolID(),
                                             stop_info_sp->GetDescription());
                            stop_looks_like_crash = true;
                            break;
                        }
                    }
                }
                
                if (stop_looks_like_crash)
                {
                    if (log)
                        log->PutCString ("ProcessGDBRemote::DoDestroy() - Stopped at a breakpoint, continue and then kill.");
                    m_destroy_tried_resuming = true;
                    
                    // If we are going to run again before killing, it would be good to suspend all the threads 
                    // before resuming so they won't get into more trouble.  Sadly, for the threads stopped with
                    // the breakpoint or exception, the exception doesn't get cleared if it is suspended, so we do
                    // have to run the risk of letting those threads proceed a bit.
    
                    {
                        Mutex::Locker locker(threads.GetMutex());
                        
                        size_t num_threads = threads.GetSize();
                        for (size_t i = 0; i < num_threads; i++)
                        {
                            ThreadSP thread_sp = threads.GetThreadAtIndex(i);
                            StopInfoSP stop_info_sp = thread_sp->GetPrivateStopInfo();
                            StopReason reason = eStopReasonInvalid;
                            if (stop_info_sp)
                                reason = stop_info_sp->GetStopReason();
                            if (reason != eStopReasonBreakpoint
                                && reason != eStopReasonException)
                            {
                                if (log)
                                    log->Printf ("ProcessGDBRemote::DoDestroy() - Suspending thread: 0x%4.4" PRIx64 " before running.",
                                                 thread_sp->GetProtocolID());
                                thread_sp->SetResumeState(eStateSuspended);
                            }
                        }
                    }
                    Resume ();
                    return Destroy();
                }
            }
        }
    }
    
    // Interrupt if our inferior is running...
    int exit_status = SIGABRT;
    std::string exit_string;

    if (m_gdb_comm.IsConnected())
    {
        if (m_public_state.GetValue() != eStateAttaching)
        {

            StringExtractorGDBRemote response;
            bool send_async = true;
            const uint32_t old_packet_timeout = m_gdb_comm.SetPacketTimeout (3);

            if (m_gdb_comm.SendPacketAndWaitForResponse("k", 1, response, send_async) == GDBRemoteCommunication::PacketResult::Success)
            {
                char packet_cmd = response.GetChar(0);

                if (packet_cmd == 'W' || packet_cmd == 'X')
                {
#if defined(__APPLE__)
                    // For Native processes on Mac OS X, we launch through the Host Platform, then hand the process off
                    // to debugserver, which becomes the parent process through "PT_ATTACH".  Then when we go to kill
                    // the process on Mac OS X we call ptrace(PT_KILL) to kill it, then we call waitpid which returns
                    // with no error and the correct status.  But amusingly enough that doesn't seem to actually reap
                    // the process, but instead it is left around as a Zombie.  Probably the kernel is in the process of
                    // switching ownership back to lldb which was the original parent, and gets confused in the handoff.
                    // Anyway, so call waitpid here to finally reap it.
                    PlatformSP platform_sp(GetTarget().GetPlatform());
                    if (platform_sp && platform_sp->IsHost())
                    {
                        int status;
                        ::pid_t reap_pid;
                        reap_pid = waitpid (GetID(), &status, WNOHANG);
                        if (log)
                            log->Printf ("Reaped pid: %d, status: %d.\n", reap_pid, status);
                    }
#endif
                    SetLastStopPacket (response);
                    ClearThreadIDList ();
                    exit_status = response.GetHexU8();
                }
                else
                {
                    if (log)
                        log->Printf ("ProcessGDBRemote::DoDestroy - got unexpected response to k packet: %s", response.GetStringRef().c_str());
                    exit_string.assign("got unexpected response to k packet: ");
                    exit_string.append(response.GetStringRef());
                }
            }
            else
            {
                if (log)
                    log->Printf ("ProcessGDBRemote::DoDestroy - failed to send k packet");
                exit_string.assign("failed to send the k packet");
            }

            m_gdb_comm.SetPacketTimeout(old_packet_timeout);
        }
        else
        {
            if (log)
                log->Printf ("ProcessGDBRemote::DoDestroy - killed or interrupted while attaching");
            exit_string.assign ("killed or interrupted while attaching.");
        }
    }
    else
    {
        // If we missed setting the exit status on the way out, do it here.
        // NB set exit status can be called multiple times, the first one sets the status.
        exit_string.assign("destroying when not connected to debugserver");
    }

    SetExitStatus(exit_status, exit_string.c_str());

    StopAsyncThread ();
    KillDebugserverProcess ();
    return error;
}

void
ProcessGDBRemote::SetLastStopPacket (const StringExtractorGDBRemote &response)
{
    lldb_private::Mutex::Locker locker (m_last_stop_packet_mutex);
    const bool did_exec = response.GetStringRef().find(";reason:exec;") != std::string::npos;
    if (did_exec)
    {
        Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_PROCESS));
        if (log)
            log->Printf ("ProcessGDBRemote::SetLastStopPacket () - detected exec");

        m_thread_list_real.Clear();
        m_thread_list.Clear();
        BuildDynamicRegisterInfo (true);
        m_gdb_comm.ResetDiscoverableSettings();
    }
    m_last_stop_packet = response;
}


//------------------------------------------------------------------
// Process Queries
//------------------------------------------------------------------

bool
ProcessGDBRemote::IsAlive ()
{
    return m_gdb_comm.IsConnected() && m_private_state.GetValue() != eStateExited;
}

addr_t
ProcessGDBRemote::GetImageInfoAddress()
{
    return m_gdb_comm.GetShlibInfoAddr();
}

//------------------------------------------------------------------
// Process Memory
//------------------------------------------------------------------
size_t
ProcessGDBRemote::DoReadMemory (addr_t addr, void *buf, size_t size, Error &error)
{
    GetMaxMemorySize ();
    if (size > m_max_memory_size)
    {
        // Keep memory read sizes down to a sane limit. This function will be
        // called multiple times in order to complete the task by 
        // lldb_private::Process so it is ok to do this.
        size = m_max_memory_size;
    }

    char packet[64];
    int packet_len;
    bool binary_memory_read = m_gdb_comm.GetxPacketSupported();
    if (binary_memory_read)
    {
        packet_len = ::snprintf (packet, sizeof(packet), "x0x%" PRIx64 ",0x%" PRIx64, (uint64_t)addr, (uint64_t)size);
    }
    else
    {
        packet_len = ::snprintf (packet, sizeof(packet), "m%" PRIx64 ",%" PRIx64, (uint64_t)addr, (uint64_t)size);
    }
    assert (packet_len + 1 < (int)sizeof(packet));
    StringExtractorGDBRemote response;
    if (m_gdb_comm.SendPacketAndWaitForResponse(packet, packet_len, response, true) == GDBRemoteCommunication::PacketResult::Success)
    {
        if (response.IsNormalResponse())
        {
            error.Clear();
            if (binary_memory_read)
            {
                // The lower level GDBRemoteCommunication packet receive layer has already de-quoted any
                // 0x7d character escaping that was present in the packet

                size_t data_received_size = response.GetBytesLeft();
                if (data_received_size > size)
                {
                    // Don't write past the end of BUF if the remote debug server gave us too
                    // much data for some reason.
                    data_received_size = size;
                }
                memcpy (buf, response.GetStringRef().data(), data_received_size);
                return data_received_size;
            }
            else
            {
                return response.GetHexBytes(buf, size, '\xdd');
            }
        }
        else if (response.IsErrorResponse())
            error.SetErrorStringWithFormat("memory read failed for 0x%" PRIx64, addr);
        else if (response.IsUnsupportedResponse())
            error.SetErrorStringWithFormat("GDB server does not support reading memory");
        else
            error.SetErrorStringWithFormat("unexpected response to GDB server memory read packet '%s': '%s'", packet, response.GetStringRef().c_str());
    }
    else
    {
        error.SetErrorStringWithFormat("failed to send packet: '%s'", packet);
    }
    return 0;
}

size_t
ProcessGDBRemote::DoWriteMemory (addr_t addr, const void *buf, size_t size, Error &error)
{
    GetMaxMemorySize ();
    if (size > m_max_memory_size)
    {
        // Keep memory read sizes down to a sane limit. This function will be
        // called multiple times in order to complete the task by 
        // lldb_private::Process so it is ok to do this.
        size = m_max_memory_size;
    }

    StreamString packet;
    packet.Printf("M%" PRIx64 ",%" PRIx64 ":", addr, (uint64_t)size);
    packet.PutBytesAsRawHex8(buf, size, lldb::endian::InlHostByteOrder(), lldb::endian::InlHostByteOrder());
    StringExtractorGDBRemote response;
    if (m_gdb_comm.SendPacketAndWaitForResponse(packet.GetData(), packet.GetSize(), response, true) == GDBRemoteCommunication::PacketResult::Success)
    {
        if (response.IsOKResponse())
        {
            error.Clear();
            return size;
        }
        else if (response.IsErrorResponse())
            error.SetErrorStringWithFormat("memory write failed for 0x%" PRIx64, addr);
        else if (response.IsUnsupportedResponse())
            error.SetErrorStringWithFormat("GDB server does not support writing memory");
        else
            error.SetErrorStringWithFormat("unexpected response to GDB server memory write packet '%s': '%s'", packet.GetString().c_str(), response.GetStringRef().c_str());
    }
    else
    {
        error.SetErrorStringWithFormat("failed to send packet: '%s'", packet.GetString().c_str());
    }
    return 0;
}

lldb::addr_t
ProcessGDBRemote::DoAllocateMemory (size_t size, uint32_t permissions, Error &error)
{
    lldb_private::Log *log (lldb_private::GetLogIfAnyCategoriesSet (LIBLLDB_LOG_PROCESS|LIBLLDB_LOG_EXPRESSIONS));
    addr_t allocated_addr = LLDB_INVALID_ADDRESS;
    
    LazyBool supported = m_gdb_comm.SupportsAllocDeallocMemory();
    switch (supported)
    {
        case eLazyBoolCalculate:
        case eLazyBoolYes:
            allocated_addr = m_gdb_comm.AllocateMemory (size, permissions);
            if (allocated_addr != LLDB_INVALID_ADDRESS || supported == eLazyBoolYes)
                return allocated_addr;

        case eLazyBoolNo:
            // Call mmap() to create memory in the inferior..
            unsigned prot = 0;
            if (permissions & lldb::ePermissionsReadable)
                prot |= eMmapProtRead;
            if (permissions & lldb::ePermissionsWritable)
                prot |= eMmapProtWrite;
            if (permissions & lldb::ePermissionsExecutable)
                prot |= eMmapProtExec;

            if (InferiorCallMmap(this, allocated_addr, 0, size, prot,
                                 eMmapFlagsAnon | eMmapFlagsPrivate, -1, 0))
                m_addr_to_mmap_size[allocated_addr] = size;
            else
            {
                allocated_addr = LLDB_INVALID_ADDRESS;
                if (log)
                    log->Printf ("ProcessGDBRemote::%s no direct stub support for memory allocation, and InferiorCallMmap also failed - is stub missing register context save/restore capability?", __FUNCTION__);
            }
            break;
    }
    
    if (allocated_addr == LLDB_INVALID_ADDRESS)
        error.SetErrorStringWithFormat("unable to allocate %" PRIu64 " bytes of memory with permissions %s", (uint64_t)size, GetPermissionsAsCString (permissions));
    else
        error.Clear();
    return allocated_addr;
}

Error
ProcessGDBRemote::GetMemoryRegionInfo (addr_t load_addr, 
                                       MemoryRegionInfo &region_info)
{
    
    Error error (m_gdb_comm.GetMemoryRegionInfo (load_addr, region_info));
    return error;
}

Error
ProcessGDBRemote::GetWatchpointSupportInfo (uint32_t &num)
{
    
    Error error (m_gdb_comm.GetWatchpointSupportInfo (num));
    return error;
}

Error
ProcessGDBRemote::GetWatchpointSupportInfo (uint32_t &num, bool& after)
{
    Error error (m_gdb_comm.GetWatchpointSupportInfo (num, after));
    return error;
}

Error
ProcessGDBRemote::DoDeallocateMemory (lldb::addr_t addr)
{
    Error error; 
    LazyBool supported = m_gdb_comm.SupportsAllocDeallocMemory();

    switch (supported)
    {
        case eLazyBoolCalculate:
            // We should never be deallocating memory without allocating memory 
            // first so we should never get eLazyBoolCalculate
            error.SetErrorString ("tried to deallocate memory without ever allocating memory");
            break;

        case eLazyBoolYes:
            if (!m_gdb_comm.DeallocateMemory (addr))
                error.SetErrorStringWithFormat("unable to deallocate memory at 0x%" PRIx64, addr);
            break;
            
        case eLazyBoolNo:
            // Call munmap() to deallocate memory in the inferior..
            {
                MMapMap::iterator pos = m_addr_to_mmap_size.find(addr);
                if (pos != m_addr_to_mmap_size.end() &&
                    InferiorCallMunmap(this, addr, pos->second))
                    m_addr_to_mmap_size.erase (pos);
                else
                    error.SetErrorStringWithFormat("unable to deallocate memory at 0x%" PRIx64, addr);
            }
            break;
    }

    return error;
}


//------------------------------------------------------------------
// Process STDIO
//------------------------------------------------------------------
size_t
ProcessGDBRemote::PutSTDIN (const char *src, size_t src_len, Error &error)
{
    if (m_stdio_communication.IsConnected())
    {
        ConnectionStatus status;
        m_stdio_communication.Write(src, src_len, status, NULL);
    }
    return 0;
}

Error
ProcessGDBRemote::EnableBreakpointSite (BreakpointSite *bp_site)
{
    Error error;
    assert(bp_site != NULL);

    // Get logging info
    Log *log(ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_BREAKPOINTS));
    user_id_t site_id = bp_site->GetID();

    // Get the breakpoint address
    const addr_t addr = bp_site->GetLoadAddress();

    // Log that a breakpoint was requested
    if (log)
        log->Printf("ProcessGDBRemote::EnableBreakpointSite (size_id = %" PRIu64 ") address = 0x%" PRIx64, site_id, (uint64_t)addr);

    // Breakpoint already exists and is enabled
    if (bp_site->IsEnabled())
    {
        if (log)
            log->Printf("ProcessGDBRemote::EnableBreakpointSite (size_id = %" PRIu64 ") address = 0x%" PRIx64 " -- SUCCESS (already enabled)", site_id, (uint64_t)addr);
        return error;
    }

    // Get the software breakpoint trap opcode size
    const size_t bp_op_size = GetSoftwareBreakpointTrapOpcode(bp_site);

    // SupportsGDBStoppointPacket() simply checks a boolean, indicating if this breakpoint type
    // is supported by the remote stub. These are set to true by default, and later set to false
    // only after we receive an unimplemented response when sending a breakpoint packet. This means
    // initially that unless we were specifically instructed to use a hardware breakpoint, LLDB will
    // attempt to set a software breakpoint. HardwareRequired() also queries a boolean variable which
    // indicates if the user specifically asked for hardware breakpoints.  If true then we will
    // skip over software breakpoints.
    if (m_gdb_comm.SupportsGDBStoppointPacket(eBreakpointSoftware) && (!bp_site->HardwareRequired()))
    {
        // Try to send off a software breakpoint packet ($Z0)
        if (m_gdb_comm.SendGDBStoppointTypePacket(eBreakpointSoftware, true, addr, bp_op_size) == 0)
        {
            // The breakpoint was placed successfully
            bp_site->SetEnabled(true);
            bp_site->SetType(BreakpointSite::eExternal);
            return error;
        }

        // SendGDBStoppointTypePacket() will return an error if it was unable to set this
        // breakpoint. We need to differentiate between a error specific to placing this breakpoint
        // or if we have learned that this breakpoint type is unsupported. To do this, we
        // must test the support boolean for this breakpoint type to see if it now indicates that
        // this breakpoint type is unsupported.  If they are still supported then we should return
        // with the error code.  If they are now unsupported, then we would like to fall through
        // and try another form of breakpoint.
        if (m_gdb_comm.SupportsGDBStoppointPacket(eBreakpointSoftware))
            return error;

        // We reach here when software breakpoints have been found to be unsupported. For future
        // calls to set a breakpoint, we will not attempt to set a breakpoint with a type that is
        // known not to be supported.
        if (log)
            log->Printf("Software breakpoints are unsupported");

        // So we will fall through and try a hardware breakpoint
    }

    // The process of setting a hardware breakpoint is much the same as above.  We check the
    // supported boolean for this breakpoint type, and if it is thought to be supported then we
    // will try to set this breakpoint with a hardware breakpoint.
    if (m_gdb_comm.SupportsGDBStoppointPacket(eBreakpointHardware))
    {
        // Try to send off a hardware breakpoint packet ($Z1)
        if (m_gdb_comm.SendGDBStoppointTypePacket(eBreakpointHardware, true, addr, bp_op_size) == 0)
        {
            // The breakpoint was placed successfully
            bp_site->SetEnabled(true);
            bp_site->SetType(BreakpointSite::eHardware);
            return error;
        }

        // Check if the error was something other then an unsupported breakpoint type
        if (m_gdb_comm.SupportsGDBStoppointPacket(eBreakpointHardware))
        {
            // Unable to set this hardware breakpoint
            error.SetErrorString("failed to set hardware breakpoint (hardware breakpoint resources might be exhausted or unavailable)");
            return error;
        }

        // We will reach here when the stub gives an unsupported response to a hardware breakpoint
        if (log)
            log->Printf("Hardware breakpoints are unsupported");

        // Finally we will falling through to a #trap style breakpoint
    }

    // Don't fall through when hardware breakpoints were specifically requested
    if (bp_site->HardwareRequired())
    {
        error.SetErrorString("hardware breakpoints are not supported");
        return error;
    }

    // As a last resort we want to place a manual breakpoint. An instruction
    // is placed into the process memory using memory write packets.
    return EnableSoftwareBreakpoint(bp_site);
}

Error
ProcessGDBRemote::DisableBreakpointSite (BreakpointSite *bp_site)
{
    Error error;
    assert (bp_site != NULL);
    addr_t addr = bp_site->GetLoadAddress();
    user_id_t site_id = bp_site->GetID();
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_BREAKPOINTS));
    if (log)
        log->Printf ("ProcessGDBRemote::DisableBreakpointSite (site_id = %" PRIu64 ") addr = 0x%8.8" PRIx64, site_id, (uint64_t)addr);

    if (bp_site->IsEnabled())
    {
        const size_t bp_op_size = GetSoftwareBreakpointTrapOpcode (bp_site);

        BreakpointSite::Type bp_type = bp_site->GetType();
        switch (bp_type)
        {
        case BreakpointSite::eSoftware:
            error = DisableSoftwareBreakpoint (bp_site);
            break;

        case BreakpointSite::eHardware:
            if (m_gdb_comm.SendGDBStoppointTypePacket(eBreakpointHardware, false, addr, bp_op_size))
                error.SetErrorToGenericError();
            break;

        case BreakpointSite::eExternal:
            {
                GDBStoppointType stoppoint_type;
                if (bp_site->IsHardware())
                    stoppoint_type = eBreakpointHardware;
                else
                    stoppoint_type = eBreakpointSoftware;
                
                if (m_gdb_comm.SendGDBStoppointTypePacket(stoppoint_type, false, addr, bp_op_size))
                error.SetErrorToGenericError();
            }
            break;
        }
        if (error.Success())
            bp_site->SetEnabled(false);
    }
    else
    {
        if (log)
            log->Printf ("ProcessGDBRemote::DisableBreakpointSite (site_id = %" PRIu64 ") addr = 0x%8.8" PRIx64 " -- SUCCESS (already disabled)", site_id, (uint64_t)addr);
        return error;
    }

    if (error.Success())
        error.SetErrorToGenericError();
    return error;
}

// Pre-requisite: wp != NULL.
static GDBStoppointType
GetGDBStoppointType (Watchpoint *wp)
{
    assert(wp);
    bool watch_read = wp->WatchpointRead();
    bool watch_write = wp->WatchpointWrite();

    // watch_read and watch_write cannot both be false.
    assert(watch_read || watch_write);
    if (watch_read && watch_write)
        return eWatchpointReadWrite;
    else if (watch_read)
        return eWatchpointRead;
    else // Must be watch_write, then.
        return eWatchpointWrite;
}

Error
ProcessGDBRemote::EnableWatchpoint (Watchpoint *wp, bool notify)
{
    Error error;
    if (wp)
    {
        user_id_t watchID = wp->GetID();
        addr_t addr = wp->GetLoadAddress();
        Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_WATCHPOINTS));
        if (log)
            log->Printf ("ProcessGDBRemote::EnableWatchpoint(watchID = %" PRIu64 ")", watchID);
        if (wp->IsEnabled())
        {
            if (log)
                log->Printf("ProcessGDBRemote::EnableWatchpoint(watchID = %" PRIu64 ") addr = 0x%8.8" PRIx64 ": watchpoint already enabled.", watchID, (uint64_t)addr);
            return error;
        }

        GDBStoppointType type = GetGDBStoppointType(wp);
        // Pass down an appropriate z/Z packet...
        if (m_gdb_comm.SupportsGDBStoppointPacket (type))
        {
            if (m_gdb_comm.SendGDBStoppointTypePacket(type, true, addr, wp->GetByteSize()) == 0)
            {
                wp->SetEnabled(true, notify);
                return error;
            }
            else
                error.SetErrorString("sending gdb watchpoint packet failed");
        }
        else
            error.SetErrorString("watchpoints not supported");
    }
    else
    {
        error.SetErrorString("Watchpoint argument was NULL.");
    }
    if (error.Success())
        error.SetErrorToGenericError();
    return error;
}

Error
ProcessGDBRemote::DisableWatchpoint (Watchpoint *wp, bool notify)
{
    Error error;
    if (wp)
    {
        user_id_t watchID = wp->GetID();

        Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_WATCHPOINTS));

        addr_t addr = wp->GetLoadAddress();

        if (log)
            log->Printf ("ProcessGDBRemote::DisableWatchpoint (watchID = %" PRIu64 ") addr = 0x%8.8" PRIx64, watchID, (uint64_t)addr);

        if (!wp->IsEnabled())
        {
            if (log)
                log->Printf ("ProcessGDBRemote::DisableWatchpoint (watchID = %" PRIu64 ") addr = 0x%8.8" PRIx64 " -- SUCCESS (already disabled)", watchID, (uint64_t)addr);
            // See also 'class WatchpointSentry' within StopInfo.cpp.
            // This disabling attempt might come from the user-supplied actions, we'll route it in order for
            // the watchpoint object to intelligently process this action.
            wp->SetEnabled(false, notify);
            return error;
        }
        
        if (wp->IsHardware())
        {
            GDBStoppointType type = GetGDBStoppointType(wp);
            // Pass down an appropriate z/Z packet...
            if (m_gdb_comm.SendGDBStoppointTypePacket(type, false, addr, wp->GetByteSize()) == 0)
            {
                wp->SetEnabled(false, notify);
                return error;
            }
            else
                error.SetErrorString("sending gdb watchpoint packet failed"); 
        }
        // TODO: clear software watchpoints if we implement them
    }
    else
    {
        error.SetErrorString("Watchpoint argument was NULL.");
    }
    if (error.Success())
        error.SetErrorToGenericError();
    return error;
}

void
ProcessGDBRemote::Clear()
{
    m_flags = 0;
    m_thread_list_real.Clear();
    m_thread_list.Clear();
}

Error
ProcessGDBRemote::DoSignal (int signo)
{
    Error error;
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_PROCESS));
    if (log)
        log->Printf ("ProcessGDBRemote::DoSignal (signal = %d)", signo);

    if (!m_gdb_comm.SendAsyncSignal (signo))
        error.SetErrorStringWithFormat("failed to send signal %i", signo);
    return error;
}

Error
ProcessGDBRemote::LaunchAndConnectToDebugserver (const ProcessInfo &process_info)
{
    Error error;
    if (m_debugserver_pid == LLDB_INVALID_PROCESS_ID)
    {
        // If we locate debugserver, keep that located version around
        static FileSpec g_debugserver_file_spec;

        ProcessLaunchInfo debugserver_launch_info;
        debugserver_launch_info.SetMonitorProcessCallback (MonitorDebugserverProcess, this, false);
        debugserver_launch_info.SetUserID(process_info.GetUserID());

#if defined (__APPLE__) && (defined (__arm__) || defined (__arm64__) || defined (__aarch64__))
        // On iOS, still do a local connection using a random port
        const char *hostname = "127.0.0.1";
        uint16_t port = get_random_port ();
#else
        // Set hostname being NULL to do the reverse connect where debugserver
        // will bind to port zero and it will communicate back to us the port
        // that we will connect to
        const char *hostname = NULL;
        uint16_t port = 0;
#endif

        error = m_gdb_comm.StartDebugserverProcess (hostname,
                                                    port,
                                                    debugserver_launch_info,
                                                    port);

        if (error.Success ())
            m_debugserver_pid = debugserver_launch_info.GetProcessID();
        else
            m_debugserver_pid = LLDB_INVALID_PROCESS_ID;

        if (m_debugserver_pid != LLDB_INVALID_PROCESS_ID)
            StartAsyncThread ();
        
        if (error.Fail())
        {
            Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_PROCESS));

            if (log)
                log->Printf("failed to start debugserver process: %s", error.AsCString());
            return error;
        }
        
        if (m_gdb_comm.IsConnected())
        {
            // Finish the connection process by doing the handshake without connecting (send NULL URL)
            ConnectToDebugserver (NULL);
        }
        else
        {
            StreamString connect_url;
            connect_url.Printf("connect://%s:%u", hostname, port);
            error = ConnectToDebugserver (connect_url.GetString().c_str());
        }

    }
    return error;
}

bool
ProcessGDBRemote::MonitorDebugserverProcess
(
    void *callback_baton,
    lldb::pid_t debugserver_pid,
    bool exited,        // True if the process did exit
    int signo,          // Zero for no signal
    int exit_status     // Exit value of process if signal is zero
)
{
    // The baton is a "ProcessGDBRemote *". Now this class might be gone
    // and might not exist anymore, so we need to carefully try to get the
    // target for this process first since we have a race condition when
    // we are done running between getting the notice that the inferior 
    // process has died and the debugserver that was debugging this process.
    // In our test suite, we are also continually running process after
    // process, so we must be very careful to make sure:
    // 1 - process object hasn't been deleted already
    // 2 - that a new process object hasn't been recreated in its place

    // "debugserver_pid" argument passed in is the process ID for
    // debugserver that we are tracking...
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_PROCESS));

    ProcessGDBRemote *process = (ProcessGDBRemote *)callback_baton;

    // Get a shared pointer to the target that has a matching process pointer.
    // This target could be gone, or the target could already have a new process
    // object inside of it
    TargetSP target_sp (Debugger::FindTargetWithProcess(process));

    if (log)
        log->Printf ("ProcessGDBRemote::MonitorDebugserverProcess (baton=%p, pid=%" PRIu64 ", signo=%i (0x%x), exit_status=%i)", callback_baton, debugserver_pid, signo, signo, exit_status);

    if (target_sp)
    {
        // We found a process in a target that matches, but another thread
        // might be in the process of launching a new process that will
        // soon replace it, so get a shared pointer to the process so we
        // can keep it alive.
        ProcessSP process_sp (target_sp->GetProcessSP());
        // Now we have a shared pointer to the process that can't go away on us
        // so we now make sure it was the same as the one passed in, and also make
        // sure that our previous "process *" didn't get deleted and have a new 
        // "process *" created in its place with the same pointer. To verify this
        // we make sure the process has our debugserver process ID. If we pass all
        // of these tests, then we are sure that this process is the one we were
        // looking for.
        if (process_sp && process == process_sp.get() && process->m_debugserver_pid == debugserver_pid)
        {
            // Sleep for a half a second to make sure our inferior process has
            // time to set its exit status before we set it incorrectly when
            // both the debugserver and the inferior process shut down.
            usleep (500000);
            // If our process hasn't yet exited, debugserver might have died.
            // If the process did exit, the we are reaping it.
            const StateType state = process->GetState();
            
            if (process->m_debugserver_pid != LLDB_INVALID_PROCESS_ID &&
                state != eStateInvalid &&
                state != eStateUnloaded &&
                state != eStateExited &&
                state != eStateDetached)
            {
                char error_str[1024];
                if (signo)
                {
                    const char *signal_cstr = process->GetUnixSignals().GetSignalAsCString (signo);
                    if (signal_cstr)
                        ::snprintf (error_str, sizeof (error_str), DEBUGSERVER_BASENAME " died with signal %s", signal_cstr);
                    else
                        ::snprintf (error_str, sizeof (error_str), DEBUGSERVER_BASENAME " died with signal %i", signo);
                }
                else
                {
                    ::snprintf (error_str, sizeof (error_str), DEBUGSERVER_BASENAME " died with an exit status of 0x%8.8x", exit_status);
                }

                process->SetExitStatus (-1, error_str);
            }
            // Debugserver has exited we need to let our ProcessGDBRemote
            // know that it no longer has a debugserver instance
            process->m_debugserver_pid = LLDB_INVALID_PROCESS_ID;
        }
    }
    return true;
}

void
ProcessGDBRemote::KillDebugserverProcess ()
{
    m_gdb_comm.Disconnect();
    if (m_debugserver_pid != LLDB_INVALID_PROCESS_ID)
    {
        Host::Kill (m_debugserver_pid, SIGINT);
        m_debugserver_pid = LLDB_INVALID_PROCESS_ID;
    }
}

void
ProcessGDBRemote::Initialize()
{
    static bool g_initialized = false;

    if (g_initialized == false)
    {
        g_initialized = true;
        PluginManager::RegisterPlugin (GetPluginNameStatic(),
                                       GetPluginDescriptionStatic(),
                                       CreateInstance,
                                       DebuggerInitialize);

        Log::Callbacks log_callbacks = {
            ProcessGDBRemoteLog::DisableLog,
            ProcessGDBRemoteLog::EnableLog,
            ProcessGDBRemoteLog::ListLogCategories
        };

        Log::RegisterLogChannel (ProcessGDBRemote::GetPluginNameStatic(), log_callbacks);
    }
}

void
ProcessGDBRemote::DebuggerInitialize (lldb_private::Debugger &debugger)
{
    if (!PluginManager::GetSettingForProcessPlugin(debugger, PluginProperties::GetSettingName()))
    {
        const bool is_global_setting = true;
        PluginManager::CreateSettingForProcessPlugin (debugger,
                                                      GetGlobalPluginProperties()->GetValueProperties(),
                                                      ConstString ("Properties for the gdb-remote process plug-in."),
                                                      is_global_setting);
    }
}

bool
ProcessGDBRemote::StartAsyncThread ()
{
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_PROCESS));

    if (log)
        log->Printf ("ProcessGDBRemote::%s ()", __FUNCTION__);
    
    Mutex::Locker start_locker(m_async_thread_state_mutex);
    if (m_async_thread_state == eAsyncThreadNotStarted)
    {
        // Create a thread that watches our internal state and controls which
        // events make it to clients (into the DCProcess event queue).
        m_async_thread = Host::ThreadCreate ("<lldb.process.gdb-remote.async>", ProcessGDBRemote::AsyncThread, this, NULL);
        if (IS_VALID_LLDB_HOST_THREAD(m_async_thread))
        {
            m_async_thread_state = eAsyncThreadRunning;
            return true;
        }
        else
            return false;
    }
    else
    {
        // Somebody tried to start the async thread while it was either being started or stopped.  If the former, and
        // it started up successfully, then say all's well.  Otherwise it is an error, since we aren't going to restart it.
        if (log)
            log->Printf ("ProcessGDBRemote::%s () - Called when Async thread was in state: %d.", __FUNCTION__, m_async_thread_state);
        if (m_async_thread_state == eAsyncThreadRunning)
            return true;
        else
            return false;
    }
}

void
ProcessGDBRemote::StopAsyncThread ()
{
    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet(GDBR_LOG_PROCESS));

    if (log)
        log->Printf ("ProcessGDBRemote::%s ()", __FUNCTION__);

    Mutex::Locker start_locker(m_async_thread_state_mutex);
    if (m_async_thread_state == eAsyncThreadRunning)
    {
        m_async_broadcaster.BroadcastEvent (eBroadcastBitAsyncThreadShouldExit);
        
        //  This will shut down the async thread.
        m_gdb_comm.Disconnect();    // Disconnect from the debug server.

        // Stop the stdio thread
        if (IS_VALID_LLDB_HOST_THREAD(m_async_thread))
        {
            Host::ThreadJoin (m_async_thread, NULL, NULL);
        }
        m_async_thread_state = eAsyncThreadDone;
    }
    else
    {
        if (log)
            log->Printf ("ProcessGDBRemote::%s () - Called when Async thread was in state: %d.", __FUNCTION__, m_async_thread_state);
    }
}


thread_result_t
ProcessGDBRemote::AsyncThread (void *arg)
{
    ProcessGDBRemote *process = (ProcessGDBRemote*) arg;

    Log *log (ProcessGDBRemoteLog::GetLogIfAllCategoriesSet (GDBR_LOG_PROCESS));
    if (log)
        log->Printf ("ProcessGDBRemote::%s (arg = %p, pid = %" PRIu64 ") thread starting...", __FUNCTION__, arg, process->GetID());

    Listener listener ("ProcessGDBRemote::AsyncThread");
    EventSP event_sp;
    const uint32_t desired_event_mask = eBroadcastBitAsyncContinue |
                                        eBroadcastBitAsyncThreadShouldExit;

    if (listener.StartListeningForEvents (&process->m_async_broadcaster, desired_event_mask) == desired_event_mask)
    {
        listener.StartListeningForEvents (&process->m_gdb_comm, Communication::eBroadcastBitReadThreadDidExit);
    
        bool done = false;
        while (!done)
        {
            if (log)
                log->Printf ("ProcessGDBRemote::%s (arg = %p, pid = %" PRIu64 ") listener.WaitForEvent (NULL, event_sp)...", __FUNCTION__, arg, process->GetID());
            if (listener.WaitForEvent (NULL, event_sp))
            {
                const uint32_t event_type = event_sp->GetType();
                if (event_sp->BroadcasterIs (&process->m_async_broadcaster))
                {
                    if (log)
                        log->Printf ("ProcessGDBRemote::%s (arg = %p, pid = %" PRIu64 ") Got an event of type: %d...", __FUNCTION__, arg, process->GetID(), event_type);

                    switch (event_type)
                    {
                        case eBroadcastBitAsyncContinue:
                            {
                                const EventDataBytes *continue_packet = EventDataBytes::GetEventDataFromEvent(event_sp.get());

                                if (continue_packet)
                                {
                                    const char *continue_cstr = (const char *)continue_packet->GetBytes ();
                                    const size_t continue_cstr_len = continue_packet->GetByteSize ();
                                    if (log)
                                        log->Printf ("ProcessGDBRemote::%s (arg = %p, pid = %" PRIu64 ") got eBroadcastBitAsyncContinue: %s", __FUNCTION__, arg, process->GetID(), continue_cstr);

                                    if (::strstr (continue_cstr, "vAttach") == NULL)
                                        process->SetPrivateState(eStateRunning);
                                    StringExtractorGDBRemote response;
                                    StateType stop_state = process->GetGDBRemote().SendContinuePacketAndWaitForResponse (process, continue_cstr, continue_cstr_len, response);

                                    // We need to immediately clear the thread ID list so we are sure to get a valid list of threads.
                                    // The thread ID list might be contained within the "response", or the stop reply packet that
                                    // caused the stop. So clear it now before we give the stop reply packet to the process
                                    // using the process->SetLastStopPacket()...
                                    process->ClearThreadIDList ();

                                    switch (stop_state)
                                    {
                                    case eStateStopped:
                                    case eStateCrashed:
                                    case eStateSuspended:
                                        process->SetLastStopPacket (response);
                                        process->SetPrivateState (stop_state);
                                        break;

                                    case eStateExited:
                                    {
                                        process->SetLastStopPacket (response);
                                        process->ClearThreadIDList();
                                        response.SetFilePos(1);
                                        
                                        int exit_status = response.GetHexU8();
                                        const char *desc_cstr = NULL;
                                        StringExtractor extractor;
                                        std::string desc_string;
                                        if (response.GetBytesLeft() > 0 && response.GetChar('-') == ';')
                                        {
                                            std::string desc_token;
                                            while (response.GetNameColonValue (desc_token, desc_string))
                                            {
                                                if (desc_token == "description")
                                                {
                                                    extractor.GetStringRef().swap(desc_string);
                                                    extractor.SetFilePos(0);
                                                    extractor.GetHexByteString (desc_string);
                                                    desc_cstr = desc_string.c_str();
                                                }
                                            }
                                        }
                                        process->SetExitStatus(exit_status, desc_cstr);
                                        done = true;
                                        break;
                                    }
                                    case eStateInvalid:
                                        process->SetExitStatus(-1, "lost connection");
                                        break;

                                    default:
                                        process->SetPrivateState (stop_state);
                                        break;
                                    }
                                }
                            }
                            break;

                        case eBroadcastBitAsyncThreadShouldExit:
                            if (log)
                                log->Printf ("ProcessGDBRemote::%s (arg = %p, pid = %" PRIu64 ") got eBroadcastBitAsyncThreadShouldExit...", __FUNCTION__, arg, process->GetID());
                            done = true;
                            break;

                        default:
                            if (log)
                                log->Printf ("ProcessGDBRemote::%s (arg = %p, pid = %" PRIu64 ") got unknown event 0x%8.8x", __FUNCTION__, arg, process->GetID(), event_type);
                            done = true;
                            break;
                    }
                }
                else if (event_sp->BroadcasterIs (&process->m_gdb_comm))
                {
                    if (event_type & Communication::eBroadcastBitReadThreadDidExit)
                    {
                        process->SetExitStatus (-1, "lost connection");
                        done = true;
                    }
                }
            }
            else
            {
                if (log)
                    log->Printf ("ProcessGDBRemote::%s (arg = %p, pid = %" PRIu64 ") listener.WaitForEvent (NULL, event_sp) => false", __FUNCTION__, arg, process->GetID());
                done = true;
            }
        }
    }

    if (log)
        log->Printf ("ProcessGDBRemote::%s (arg = %p, pid = %" PRIu64 ") thread exiting...", __FUNCTION__, arg, process->GetID());

    process->m_async_thread = LLDB_INVALID_HOST_THREAD;
    return NULL;
}

//uint32_t
//ProcessGDBRemote::ListProcessesMatchingName (const char *name, StringList &matches, std::vector<lldb::pid_t> &pids)
//{
//    // If we are planning to launch the debugserver remotely, then we need to fire up a debugserver
//    // process and ask it for the list of processes. But if we are local, we can let the Host do it.
//    if (m_local_debugserver)
//    {
//        return Host::ListProcessesMatchingName (name, matches, pids);
//    }
//    else 
//    {
//        // FIXME: Implement talking to the remote debugserver.
//        return 0;
//    }
//
//}
//
bool
ProcessGDBRemote::NewThreadNotifyBreakpointHit (void *baton,
                             lldb_private::StoppointCallbackContext *context,
                             lldb::user_id_t break_id,
                             lldb::user_id_t break_loc_id)
{
    // I don't think I have to do anything here, just make sure I notice the new thread when it starts to 
    // run so I can stop it if that's what I want to do.
    Log *log (lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_STEP));
    if (log)
        log->Printf("Hit New Thread Notification breakpoint.");
    return false;
}


bool
ProcessGDBRemote::StartNoticingNewThreads()
{
    Log *log (lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_STEP));
    if (m_thread_create_bp_sp)
    {
        if (log && log->GetVerbose())
            log->Printf("Enabled noticing new thread breakpoint.");
        m_thread_create_bp_sp->SetEnabled(true);
    }
    else
    {
        PlatformSP platform_sp (m_target.GetPlatform());
        if (platform_sp)
        {
            m_thread_create_bp_sp = platform_sp->SetThreadCreationBreakpoint(m_target);
            if (m_thread_create_bp_sp)
            {
                if (log && log->GetVerbose())
                    log->Printf("Successfully created new thread notification breakpoint %i", m_thread_create_bp_sp->GetID());
                m_thread_create_bp_sp->SetCallback (ProcessGDBRemote::NewThreadNotifyBreakpointHit, this, true);
            }
            else
            {
                if (log)
                    log->Printf("Failed to create new thread notification breakpoint.");
            }
        }
    }
    return m_thread_create_bp_sp.get() != NULL;
}

bool
ProcessGDBRemote::StopNoticingNewThreads()
{   
    Log *log (lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_STEP));
    if (log && log->GetVerbose())
        log->Printf ("Disabling new thread notification breakpoint.");

    if (m_thread_create_bp_sp)
        m_thread_create_bp_sp->SetEnabled(false);

    return true;
}
    
lldb_private::DynamicLoader *
ProcessGDBRemote::GetDynamicLoader ()
{
    if (m_dyld_ap.get() == NULL)
        m_dyld_ap.reset (DynamicLoader::FindPlugin(this, NULL));
    return m_dyld_ap.get();
}

Error
ProcessGDBRemote::SendEventData(const char *data)
{
    int return_value;
    bool was_supported;
    
    Error error;
    
    return_value = m_gdb_comm.SendLaunchEventDataPacket (data, &was_supported);
    if (return_value != 0)
    {
        if (!was_supported)
            error.SetErrorString("Sending events is not supported for this process.");
        else
            error.SetErrorStringWithFormat("Error sending event data: %d.", return_value);
    }
    return error;
}

const DataBufferSP
ProcessGDBRemote::GetAuxvData()
{
    DataBufferSP buf;
    if (m_gdb_comm.GetQXferAuxvReadSupported())
    {
        std::string response_string;
        if (m_gdb_comm.SendPacketsAndConcatenateResponses("qXfer:auxv:read::", response_string) == GDBRemoteCommunication::PacketResult::Success)
            buf.reset(new DataBufferHeap(response_string.c_str(), response_string.length()));
    }
    return buf;
}

StructuredData::ObjectSP
ProcessGDBRemote::GetExtendedInfoForThread (lldb::tid_t tid)
{
    StructuredData::ObjectSP object_sp;

    if (m_gdb_comm.GetThreadExtendedInfoSupported())
    {
        StructuredData::ObjectSP args_dict(new StructuredData::Dictionary());
        SystemRuntime *runtime = GetSystemRuntime();
        if (runtime)
        {
            runtime->AddThreadExtendedInfoPacketHints (args_dict);
        }
        args_dict->GetAsDictionary()->AddIntegerItem ("thread", tid);

        StreamString packet;
        packet << "jThreadExtendedInfo:";
        args_dict->Dump (packet);

        // FIXME the final character of a JSON dictionary, '}', is the escape
        // character in gdb-remote binary mode.  lldb currently doesn't escape
        // these characters in its packet output -- so we add the quoted version
        // of the } character here manually in case we talk to a debugserver which
        // un-escapes the characters at packet read time.
        packet << (char) (0x7d ^ 0x20);

        StringExtractorGDBRemote response;
        if (m_gdb_comm.SendPacketAndWaitForResponse(packet.GetData(), packet.GetSize(), response, false) == GDBRemoteCommunication::PacketResult::Success)
        {
            StringExtractorGDBRemote::ResponseType response_type = response.GetResponseType();
            if (response_type == StringExtractorGDBRemote::eResponse)
            {
                if (!response.Empty())
                {
                    // The packet has already had the 0x7d xor quoting stripped out at the
                    // GDBRemoteCommunication packet receive level.
                    object_sp = StructuredData::ParseJSON (response.GetStringRef());
                }
            }
        }
    }
    return object_sp;
}

// Establish the largest memory read/write payloads we should use.
// If the remote stub has a max packet size, stay under that size.
// 
// If the remote stub's max packet size is crazy large, use a 
// reasonable largeish default.
//
// If the remote stub doesn't advertise a max packet size, use a
// conservative default.

void
ProcessGDBRemote::GetMaxMemorySize()
{
    const uint64_t reasonable_largeish_default = 128 * 1024;
    const uint64_t conservative_default = 512;

    if (m_max_memory_size == 0)
    {
        uint64_t stub_max_size = m_gdb_comm.GetRemoteMaxPacketSize();
        if (stub_max_size != UINT64_MAX && stub_max_size != 0)
        {
            // Save the stub's claimed maximum packet size
            m_remote_stub_max_memory_size = stub_max_size;

            // Even if the stub says it can support ginormous packets,
            // don't exceed our reasonable largeish default packet size.
            if (stub_max_size > reasonable_largeish_default)
            {
                stub_max_size = reasonable_largeish_default;
            }

            m_max_memory_size = stub_max_size;
        }
        else
        {
            m_max_memory_size = conservative_default;
        }
    }
}

void
ProcessGDBRemote::SetUserSpecifiedMaxMemoryTransferSize (uint64_t user_specified_max)
{
    if (user_specified_max != 0)
    {
        GetMaxMemorySize ();

        if (m_remote_stub_max_memory_size != 0)
        {
            if (m_remote_stub_max_memory_size < user_specified_max)
            {
                m_max_memory_size = m_remote_stub_max_memory_size;   // user specified a packet size too big, go as big
                                                                     // as the remote stub says we can go.
            }
            else
            {
                m_max_memory_size = user_specified_max;             // user's packet size is good
            }
        }
        else
        {
            m_max_memory_size = user_specified_max;                 // user's packet size is probably fine
        }
    }
}

class CommandObjectProcessGDBRemotePacketHistory : public CommandObjectParsed
{
private:
    
public:
    CommandObjectProcessGDBRemotePacketHistory(CommandInterpreter &interpreter) :
    CommandObjectParsed (interpreter,
                         "process plugin packet history",
                         "Dumps the packet history buffer. ",
                         NULL)
    {
    }
    
    ~CommandObjectProcessGDBRemotePacketHistory ()
    {
    }
    
    bool
    DoExecute (Args& command, CommandReturnObject &result)
    {
        const size_t argc = command.GetArgumentCount();
        if (argc == 0)
        {
            ProcessGDBRemote *process = (ProcessGDBRemote *)m_interpreter.GetExecutionContext().GetProcessPtr();
            if (process)
            {
                process->GetGDBRemote().DumpHistory(result.GetOutputStream());
                result.SetStatus (eReturnStatusSuccessFinishResult);
                return true;
            }
        }
        else
        {
            result.AppendErrorWithFormat ("'%s' takes no arguments", m_cmd_name.c_str());
        }
        result.SetStatus (eReturnStatusFailed);
        return false;
    }
};

class CommandObjectProcessGDBRemotePacketXferSize : public CommandObjectParsed
{
private:
    
public:
    CommandObjectProcessGDBRemotePacketXferSize(CommandInterpreter &interpreter) :
    CommandObjectParsed (interpreter,
                         "process plugin packet xfer-size",
                         "Maximum size that lldb will try to read/write one one chunk.",
                         NULL)
    {
    }
    
    ~CommandObjectProcessGDBRemotePacketXferSize ()
    {
    }
    
    bool
    DoExecute (Args& command, CommandReturnObject &result)
    {
        const size_t argc = command.GetArgumentCount();
        if (argc == 0)
        {
            result.AppendErrorWithFormat ("'%s' takes an argument to specify the max amount to be transferred when reading/writing", m_cmd_name.c_str());
            result.SetStatus (eReturnStatusFailed);
            return false;
        }

        ProcessGDBRemote *process = (ProcessGDBRemote *)m_interpreter.GetExecutionContext().GetProcessPtr();
        if (process)
        {
            const char *packet_size = command.GetArgumentAtIndex(0);
            errno = 0;
            uint64_t user_specified_max = strtoul (packet_size, NULL, 10);
            if (errno == 0 && user_specified_max != 0)
            {
                process->SetUserSpecifiedMaxMemoryTransferSize (user_specified_max);
                result.SetStatus (eReturnStatusSuccessFinishResult);
                return true;
            }
        }
        result.SetStatus (eReturnStatusFailed);
        return false;
    }
};


class CommandObjectProcessGDBRemotePacketSend : public CommandObjectParsed
{
private:
    
public:
    CommandObjectProcessGDBRemotePacketSend(CommandInterpreter &interpreter) :
        CommandObjectParsed (interpreter,
                             "process plugin packet send",
                             "Send a custom packet through the GDB remote protocol and print the answer. "
                             "The packet header and footer will automatically be added to the packet prior to sending and stripped from the result.",
                             NULL)
    {
    }
    
    ~CommandObjectProcessGDBRemotePacketSend ()
    {
    }
    
    bool
    DoExecute (Args& command, CommandReturnObject &result)
    {
        const size_t argc = command.GetArgumentCount();
        if (argc == 0)
        {
            result.AppendErrorWithFormat ("'%s' takes a one or more packet content arguments", m_cmd_name.c_str());
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        
        ProcessGDBRemote *process = (ProcessGDBRemote *)m_interpreter.GetExecutionContext().GetProcessPtr();
        if (process)
        {
            for (size_t i=0; i<argc; ++ i)
            {
                const char *packet_cstr = command.GetArgumentAtIndex(0);
                bool send_async = true;
                StringExtractorGDBRemote response;
                process->GetGDBRemote().SendPacketAndWaitForResponse(packet_cstr, response, send_async);
                result.SetStatus (eReturnStatusSuccessFinishResult);
                Stream &output_strm = result.GetOutputStream();
                output_strm.Printf ("  packet: %s\n", packet_cstr);
                std::string &response_str = response.GetStringRef();
                
                if (strstr(packet_cstr, "qGetProfileData") != NULL)
                {
                    response_str = process->GetGDBRemote().HarmonizeThreadIdsForProfileData(process, response);
                }

                if (response_str.empty())
                    output_strm.PutCString ("response: \nerror: UNIMPLEMENTED\n");
                else
                    output_strm.Printf ("response: %s\n", response.GetStringRef().c_str());
            }
        }
        return true;
    }
};

class CommandObjectProcessGDBRemotePacketMonitor : public CommandObjectRaw
{
private:
    
public:
    CommandObjectProcessGDBRemotePacketMonitor(CommandInterpreter &interpreter) :
        CommandObjectRaw (interpreter,
                         "process plugin packet monitor",
                         "Send a qRcmd packet through the GDB remote protocol and print the response."
                         "The argument passed to this command will be hex encoded into a valid 'qRcmd' packet, sent and the response will be printed.",
                         NULL)
    {
    }
    
    ~CommandObjectProcessGDBRemotePacketMonitor ()
    {
    }
    
    bool
    DoExecute (const char *command, CommandReturnObject &result)
    {
        if (command == NULL || command[0] == '\0')
        {
            result.AppendErrorWithFormat ("'%s' takes a command string argument", m_cmd_name.c_str());
            result.SetStatus (eReturnStatusFailed);
            return false;
        }
        
        ProcessGDBRemote *process = (ProcessGDBRemote *)m_interpreter.GetExecutionContext().GetProcessPtr();
        if (process)
        {
            StreamString packet;
            packet.PutCString("qRcmd,");
            packet.PutBytesAsRawHex8(command, strlen(command));
            const char *packet_cstr = packet.GetString().c_str();
            
            bool send_async = true;
            StringExtractorGDBRemote response;
            process->GetGDBRemote().SendPacketAndWaitForResponse(packet_cstr, response, send_async);
            result.SetStatus (eReturnStatusSuccessFinishResult);
            Stream &output_strm = result.GetOutputStream();
            output_strm.Printf ("  packet: %s\n", packet_cstr);
            const std::string &response_str = response.GetStringRef();
            
            if (response_str.empty())
                output_strm.PutCString ("response: \nerror: UNIMPLEMENTED\n");
            else
                output_strm.Printf ("response: %s\n", response.GetStringRef().c_str());
        }
        return true;
    }
};

class CommandObjectProcessGDBRemotePacket : public CommandObjectMultiword
{
private:
    
public:
    CommandObjectProcessGDBRemotePacket(CommandInterpreter &interpreter) :
        CommandObjectMultiword (interpreter,
                                "process plugin packet",
                                "Commands that deal with GDB remote packets.",
                                NULL)
    {
        LoadSubCommand ("history", CommandObjectSP (new CommandObjectProcessGDBRemotePacketHistory (interpreter)));
        LoadSubCommand ("send", CommandObjectSP (new CommandObjectProcessGDBRemotePacketSend (interpreter)));
        LoadSubCommand ("monitor", CommandObjectSP (new CommandObjectProcessGDBRemotePacketMonitor (interpreter)));
        LoadSubCommand ("xfer-size", CommandObjectSP (new CommandObjectProcessGDBRemotePacketXferSize (interpreter)));
    }
    
    ~CommandObjectProcessGDBRemotePacket ()
    {
    }    
};

class CommandObjectMultiwordProcessGDBRemote : public CommandObjectMultiword
{
public:
    CommandObjectMultiwordProcessGDBRemote (CommandInterpreter &interpreter) :
        CommandObjectMultiword (interpreter,
                                "process plugin",
                                "A set of commands for operating on a ProcessGDBRemote process.",
                                "process plugin <subcommand> [<subcommand-options>]")
    {
        LoadSubCommand ("packet", CommandObjectSP (new CommandObjectProcessGDBRemotePacket    (interpreter)));
    }

    ~CommandObjectMultiwordProcessGDBRemote ()
    {
    }
};

CommandObject *
ProcessGDBRemote::GetPluginCommandObject()
{
    if (!m_command_sp)
        m_command_sp.reset (new CommandObjectMultiwordProcessGDBRemote (GetTarget().GetDebugger().GetCommandInterpreter()));
    return m_command_sp.get();
}
