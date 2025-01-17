//===-- MICmdCmdExec.cpp ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

//++
// File:		MICmdCmdExec.cpp
//
// Overview:	CMICmdCmdExecRun				implementation.
//				CMICmdCmdExecContinue			implementation.
//				CMICmdCmdExecNext				implementation. 
//				CMICmdCmdExecStep				implementation.
//				CMICmdCmdExecNextInstruction	implementation.
//				CMICmdCmdExecStepInstruction	implementation.
//				CMICmdCmdExecFinish				implementation.
//				CMICmdCmdExecInterrupt			implementation.
//
// Environment:	Compilers:	Visual C++ 12.
//							gcc (Ubuntu/Linaro 4.8.1-10ubuntu9) 4.8.1
//				Libraries:	See MIReadmetxt. 
//
// Copyright:	None.
//--

// Third Party Headers:
#include <lldb/API/SBCommandInterpreter.h>

// In-house headers:
#include "MICmdCmdExec.h"
#include "MICmnMIResultRecord.h"
#include "MICmnMIValueConst.h"
#include "MICmnLLDBDebugger.h"
#include "MICmnLLDBDebugSessionInfo.h"
#include "MIDriver.h"
#include "MICmdArgValNumber.h"
#include "MICmdArgValString.h"
#include "MICmdArgValThreadGrp.h"
#include "MICmdArgValOptionLong.h"
#include "MICmdArgValOptionShort.h"
#include "MICmdArgValListOfN.h"
#include "MICmnStreamStdout.h"
#include "MICmnMIOutOfBandRecord.h"

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecRun constructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecRun::CMICmdCmdExecRun( void )
{
	// Command factory matches this name with that received from the stdin stream
	m_strMiCmd = "exec-run";
	
	// Required by the CMICmdFactory when registering *this command
	m_pSelfCreatorFn = &CMICmdCmdExecRun::CreateSelf;
}

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecRun destructor.
// Type:	Overrideable.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecRun::~CMICmdCmdExecRun( void )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command does work in this function.
//			The command is likely to communicate with the LLDB SBDebugger in here.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecRun::Execute( void )
{
    const MIchar * pCmd = "run";
    CMICmnLLDBDebugSessionInfo & rSessionInfo( CMICmnLLDBDebugSessionInfo::Instance() );
    const lldb::ReturnStatus rtn = rSessionInfo.m_rLldbDebugger.GetCommandInterpreter().HandleCommand( pCmd, m_lldbResult ); MIunused( rtn );
	
	if( m_lldbResult.GetErrorSize() == 0 )
	{
		if( !CMIDriver::Instance().SetDriverStateRunningDebugging() )
		{
			const CMIUtilString & rErrMsg( CMIDriver::Instance().GetErrorDescription() );
			SetError( CMIUtilString::Format( MIRSRC( IDS_CMD_ERR_SET_NEW_DRIVER_STATE ), m_cmdData.strMiCmd.c_str(), rErrMsg.c_str() ) );
			return MIstatus::failure;
		}
	}

	 return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command prepares a MI Record Result
//			for the work carried out in the Execute().
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecRun::Acknowledge( void )
{
    if( m_lldbResult.GetErrorSize() > 0 )
    {
        const CMICmnMIValueConst miValueConst( m_lldbResult.GetError() );
        const CMICmnMIValueResult miValueResult( "message", miValueConst );
        const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Error, miValueResult );
        m_miResultRecord = miRecordResult;
    }
    else
    {
        const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Running );
        m_miResultRecord = miRecordResult;
    }
	
	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Required by the CMICmdFactory when registering *this command. The factory
//			calls this function to create an instance of *this command.
// Type:	Static method.
// Args:	None.
// Return:	CMICmdBase * - Pointer to a new command.
// Throws:	None.
//--
CMICmdBase * CMICmdCmdExecRun::CreateSelf( void )
{
	return new CMICmdCmdExecRun();
}

//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecContinue constructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecContinue::CMICmdCmdExecContinue( void )
{
	// Command factory matches this name with that received from the stdin stream
	m_strMiCmd = "exec-continue";
	
	// Required by the CMICmdFactory when registering *this command
	m_pSelfCreatorFn = &CMICmdCmdExecContinue::CreateSelf;
}

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecContinue destructor.
// Type:	Overrideable.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecContinue::~CMICmdCmdExecContinue( void )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command does work in this function.
//			The command is likely to communicate with the LLDB SBDebugger in here.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecContinue::Execute( void )
{
	const MIchar * pCmd = "continue";
	CMICmnLLDBDebugSessionInfo & rSessionInfo( CMICmnLLDBDebugSessionInfo::Instance() );
	const lldb::ReturnStatus rtn = rSessionInfo.m_rLldbDebugger.GetCommandInterpreter().HandleCommand( pCmd, m_lldbResult ); MIunused( rtn );
	

	if( m_lldbResult.GetErrorSize() == 0 )
	{
		// CODETAG_DEBUG_SESSION_RUNNING_PROG_RECEIVED_SIGINT_PAUSE_PROGRAM
		if( !CMIDriver::Instance().SetDriverStateRunningDebugging() )
		{
			const CMIUtilString & rErrMsg( CMIDriver::Instance().GetErrorDescription() );
			SetError( CMIUtilString::Format( MIRSRC( IDS_CMD_ERR_SET_NEW_DRIVER_STATE ), m_cmdData.strMiCmd.c_str(), rErrMsg.c_str() ) );
			return MIstatus::failure;
		}
	}
	else
	{
		// ToDo: Re-evaluate if this is required when application near finished as this is parsing LLDB error message
		// which seems a hack and is code brittle
		const MIchar * pLldbErr = m_lldbResult.GetError();
		const CMIUtilString strLldbMsg( CMIUtilString( pLldbErr ).StripCREndOfLine() );
		if( strLldbMsg == "error: Process must be launched." )
		{
			CMIDriver::Instance().SetExitApplicationFlag( true );
		}
	}

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command prepares a MI Record Result
//			for the work carried out in the Execute().
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecContinue::Acknowledge( void )
{
	if( m_lldbResult.GetErrorSize() > 0 )
	{
		const CMICmnMIValueConst miValueConst( m_lldbResult.GetError() );
		const CMICmnMIValueResult miValueResult( "message", miValueConst );
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Error, miValueResult );
		m_miResultRecord = miRecordResult;
	}
	else
	{
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Running );
		m_miResultRecord = miRecordResult;
	}
		
	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Required by the CMICmdFactory when registering *this command. The factory
//			calls this function to create an instance of *this command.
// Type:	Static method.
// Args:	None.
// Return:	CMICmdBase * - Pointer to a new command.
// Throws:	None.
//--
CMICmdBase * CMICmdCmdExecContinue::CreateSelf( void )
{
	return new CMICmdCmdExecContinue();
}

//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecNext constructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecNext::CMICmdCmdExecNext( void )
:	m_constStrArgThread( "thread" )
,	m_constStrArgNumber( "number" )
{
	// Command factory matches this name with that received from the stdin stream
	m_strMiCmd = "exec-next";
	
	// Required by the CMICmdFactory when registering *this command
	m_pSelfCreatorFn = &CMICmdCmdExecNext::CreateSelf;
}

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecNext destructor.
// Type:	Overrideable.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecNext::~CMICmdCmdExecNext( void )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The parses the command line options 
//			arguments to extract values for each of those arguments.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecNext::ParseArgs( void )
{
	bool bOk = m_setCmdArgs.Add( *(new CMICmdArgValOptionLong( m_constStrArgThread, true, true, CMICmdArgValListBase::eArgValType_Number, 1 ) ) );
	bOk = bOk && m_setCmdArgs.Add( *(new CMICmdArgValNumber( m_constStrArgNumber, false, false ) ) );
	return (bOk && ParseValidateCmdOptions() );
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command does work in this function.
//			The command is likely to communicate with the LLDB SBDebugger in here.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecNext::Execute( void )
{
	CMICMDBASE_GETOPTION( pArgThread, OptionLong, m_constStrArgThread );

	// Retrieve the --thread option's thread ID (only 1)
	MIuint64 nThreadId = UINT64_MAX;
	if( !pArgThread->GetExpectedOption< CMICmdArgValNumber, MIuint64 >( nThreadId ) )
	{
		SetError( CMIUtilString::Format( MIRSRC( IDS_CMD_ERR_THREAD_INVALID ), m_cmdData.strMiCmd.c_str(), m_constStrArgThread.c_str() ) );
		return MIstatus::failure;
	}

	CMICmnLLDBDebugSessionInfo & rSessionInfo( CMICmnLLDBDebugSessionInfo::Instance() );
	lldb::SBDebugger & rDebugger = rSessionInfo.m_rLldbDebugger;
	CMIUtilString strCmd( "thread step-over" );
	if( nThreadId != UINT64_MAX )
		strCmd += CMIUtilString::Format( " %llu", nThreadId );
	rDebugger.GetCommandInterpreter().HandleCommand( strCmd.c_str(), m_lldbResult, false );

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command prepares a MI Record Result
//			for the work carried out in the Execute().
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecNext::Acknowledge( void )
{
	if( m_lldbResult.GetErrorSize() > 0 )
	{
		const MIchar * pLldbErr = m_lldbResult.GetError(); MIunused( pLldbErr );
		const CMICmnMIValueConst miValueConst( m_lldbResult.GetError() );
		const CMICmnMIValueResult miValueResult( "message", miValueConst );
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Error, miValueResult );
		m_miResultRecord = miRecordResult;
	}
	else
	{
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Running );
		m_miResultRecord = miRecordResult;
	}

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Required by the CMICmdFactory when registering *this command. The factory
//			calls this function to create an instance of *this command.
// Type:	Static method.
// Args:	None.
// Return:	CMICmdBase * - Pointer to a new command.
// Throws:	None.
//--
CMICmdBase * CMICmdCmdExecNext::CreateSelf( void )
{
	return new CMICmdCmdExecNext();
}

//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecStep constructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecStep::CMICmdCmdExecStep( void )
:	m_constStrArgThread( "thread" )
,	m_constStrArgNumber( "number" )
{
	// Command factory matches this name with that received from the stdin stream
	m_strMiCmd = "exec-step";
	
	// Required by the CMICmdFactory when registering *this command
	m_pSelfCreatorFn = &CMICmdCmdExecStep::CreateSelf;
}

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecStep destructor.
// Type:	Overrideable.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecStep::~CMICmdCmdExecStep( void )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The parses the command line options 
//			arguments to extract values for each of those arguments.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecStep::ParseArgs( void )
{
	bool bOk = m_setCmdArgs.Add( *(new CMICmdArgValOptionLong( m_constStrArgThread, true, true, CMICmdArgValListBase::eArgValType_Number, 1 ) ) );
	bOk = bOk && m_setCmdArgs.Add( *(new CMICmdArgValNumber( m_constStrArgNumber, false, false ) ) );
	return (bOk && ParseValidateCmdOptions() );
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command does work in this function.
//			The command is likely to communicate with the LLDB SBDebugger in here.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecStep::Execute( void )
{
	CMICMDBASE_GETOPTION( pArgThread, OptionLong, m_constStrArgThread );

	// Retrieve the --thread option's thread ID (only 1)
	MIuint64 nThreadId = UINT64_MAX;
	if( !pArgThread->GetExpectedOption< CMICmdArgValNumber, MIuint64 >( nThreadId ) )
	{
		SetError( CMIUtilString::Format( MIRSRC( IDS_CMD_ERR_OPTION_NOT_FOUND ), m_cmdData.strMiCmd.c_str(), m_constStrArgThread.c_str() ) );
		return MIstatus::failure;
	}

	CMICmnLLDBDebugSessionInfo & rSessionInfo( CMICmnLLDBDebugSessionInfo::Instance() );
	lldb::SBDebugger & rDebugger = rSessionInfo.m_rLldbDebugger;
	CMIUtilString strCmd( "thread step-in" );
	if( nThreadId != UINT64_MAX )
		strCmd += CMIUtilString::Format( " %llu", nThreadId );
	rDebugger.GetCommandInterpreter().HandleCommand( strCmd.c_str(), m_lldbResult, false );

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command prepares a MI Record Result
//			for the work carried out in the Execute().
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecStep::Acknowledge( void )
{
	if( m_lldbResult.GetErrorSize() > 0 )
	{
		const MIchar * pLldbErr = m_lldbResult.GetError(); MIunused( pLldbErr );
		const CMICmnMIValueConst miValueConst( m_lldbResult.GetError() );
		const CMICmnMIValueResult miValueResult( "message", miValueConst );
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Error, miValueResult );
		m_miResultRecord = miRecordResult;
	}
	else
	{
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Running );
		m_miResultRecord = miRecordResult;
	}

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Required by the CMICmdFactory when registering *this command. The factory
//			calls this function to create an instance of *this command.
// Type:	Static method.
// Args:	None.
// Return:	CMICmdBase * - Pointer to a new command.
// Throws:	None.
//--
CMICmdBase * CMICmdCmdExecStep::CreateSelf( void )
{
	return new CMICmdCmdExecStep();
}

//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecNextInstruction constructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecNextInstruction::CMICmdCmdExecNextInstruction( void )
:	m_constStrArgThread( "thread" )
,	m_constStrArgNumber( "number" )
{
	// Command factory matches this name with that received from the stdin stream
	m_strMiCmd = "exec-next-instruction";
	
	// Required by the CMICmdFactory when registering *this command
	m_pSelfCreatorFn = &CMICmdCmdExecNextInstruction::CreateSelf;
}

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecNextInstruction destructor.
// Type:	Overrideable.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecNextInstruction::~CMICmdCmdExecNextInstruction( void )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The parses the command line options 
//			arguments to extract values for each of those arguments.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecNextInstruction::ParseArgs( void )
{
	bool bOk = m_setCmdArgs.Add( *(new CMICmdArgValOptionLong( m_constStrArgThread, true, true, CMICmdArgValListBase::eArgValType_Number, 1 ) ) );
	bOk = bOk && m_setCmdArgs.Add( *(new CMICmdArgValNumber( m_constStrArgNumber, false, false ) ) );
	return (bOk && ParseValidateCmdOptions() );
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command does work in this function.
//			The command is likely to communicate with the LLDB SBDebugger in here.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecNextInstruction::Execute( void )
{
	CMICMDBASE_GETOPTION( pArgThread, OptionLong, m_constStrArgThread );

	// Retrieve the --thread option's thread ID (only 1)
	MIuint64 nThreadId = UINT64_MAX;
	if( !pArgThread->GetExpectedOption< CMICmdArgValNumber, MIuint64 >( nThreadId ) )
	{
		SetError( CMIUtilString::Format( MIRSRC( IDS_CMD_ERR_OPTION_NOT_FOUND ), m_cmdData.strMiCmd.c_str(), m_constStrArgThread.c_str() ) );
		return MIstatus::failure;
	}

	CMICmnLLDBDebugSessionInfo & rSessionInfo( CMICmnLLDBDebugSessionInfo::Instance() );
	lldb::SBDebugger & rDebugger = rSessionInfo.m_rLldbDebugger;
	CMIUtilString strCmd( "thread step-inst-over" );
	if( nThreadId != UINT64_MAX )
		strCmd += CMIUtilString::Format( " %llu", nThreadId );
	rDebugger.GetCommandInterpreter().HandleCommand( strCmd.c_str(), m_lldbResult, false );

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command prepares a MI Record Result
//			for the work carried out in the Execute().
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecNextInstruction::Acknowledge( void )
{
	if( m_lldbResult.GetErrorSize() > 0 )
	{
		const MIchar * pLldbErr = m_lldbResult.GetError(); MIunused( pLldbErr );
		const CMICmnMIValueConst miValueConst( m_lldbResult.GetError() );
		const CMICmnMIValueResult miValueResult( "message", miValueConst );
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Error, miValueResult );
		m_miResultRecord = miRecordResult;
	}
	else
	{
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Running );
		m_miResultRecord = miRecordResult;
	}

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Required by the CMICmdFactory when registering *this command. The factory
//			calls this function to create an instance of *this command.
// Type:	Static method.
// Args:	None.
// Return:	CMICmdBase * - Pointer to a new command.
// Throws:	None.
//--
CMICmdBase * CMICmdCmdExecNextInstruction::CreateSelf( void )
{
	return new CMICmdCmdExecNextInstruction();
}

//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecStepInstruction constructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecStepInstruction::CMICmdCmdExecStepInstruction( void )
:	m_constStrArgThread( "thread" )
,	m_constStrArgNumber( "number" )
{
	// Command factory matches this name with that received from the stdin stream
	m_strMiCmd = "exec-step-instruction";
	
	// Required by the CMICmdFactory when registering *this command
	m_pSelfCreatorFn = &CMICmdCmdExecStepInstruction::CreateSelf;
}

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecStepInstruction destructor.
// Type:	Overrideable.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecStepInstruction::~CMICmdCmdExecStepInstruction( void )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The parses the command line options 
//			arguments to extract values for each of those arguments.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecStepInstruction::ParseArgs( void )
{
	bool bOk = m_setCmdArgs.Add( *(new CMICmdArgValOptionLong( m_constStrArgThread, true, true, CMICmdArgValListBase::eArgValType_Number, 1 ) ) );
	bOk = bOk && m_setCmdArgs.Add( *(new CMICmdArgValNumber( m_constStrArgNumber, false, false ) ) );
	return (bOk && ParseValidateCmdOptions() );
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command does work in this function.
//			The command is likely to communicate with the LLDB SBDebugger in here.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecStepInstruction::Execute( void )
{
	CMICMDBASE_GETOPTION( pArgThread, OptionLong, m_constStrArgThread );

	// Retrieve the --thread option's thread ID (only 1)
	MIuint64 nThreadId = UINT64_MAX;
	if( !pArgThread->GetExpectedOption< CMICmdArgValNumber, MIuint64 >( nThreadId ) )
	{
		SetError( CMIUtilString::Format( MIRSRC( IDS_CMD_ERR_OPTION_NOT_FOUND ), m_cmdData.strMiCmd.c_str(), m_constStrArgThread.c_str() ) );
		return MIstatus::failure;
	}

	CMICmnLLDBDebugSessionInfo & rSessionInfo( CMICmnLLDBDebugSessionInfo::Instance() );
	lldb::SBDebugger & rDebugger = rSessionInfo.m_rLldbDebugger;
	CMIUtilString strCmd( "thread step-inst" );
	if( nThreadId != UINT64_MAX )
		strCmd += CMIUtilString::Format( " %llu", nThreadId );
	rDebugger.GetCommandInterpreter().HandleCommand( strCmd.c_str(), m_lldbResult, false );

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command prepares a MI Record Result
//			for the work carried out in the Execute().
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecStepInstruction::Acknowledge( void )
{
	if( m_lldbResult.GetErrorSize() > 0 )
	{
		const MIchar * pLldbErr = m_lldbResult.GetError(); MIunused( pLldbErr );
		const CMICmnMIValueConst miValueConst( m_lldbResult.GetError() );
		const CMICmnMIValueResult miValueResult( "message", miValueConst );
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Error, miValueResult );
		m_miResultRecord = miRecordResult;
	}
	else
	{
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Running );
		m_miResultRecord = miRecordResult;
	}

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Required by the CMICmdFactory when registering *this command. The factory
//			calls this function to create an instance of *this command.
// Type:	Static method.
// Args:	None.
// Return:	CMICmdBase * - Pointer to a new command.
// Throws:	None.
//--
CMICmdBase * CMICmdCmdExecStepInstruction::CreateSelf( void )
{
	return new CMICmdCmdExecStepInstruction();
}

//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecFinish constructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecFinish::CMICmdCmdExecFinish( void )
:	m_constStrArgThread( "thread" )
,	m_constStrArgFrame( "frame" )
{
	// Command factory matches this name with that received from the stdin stream
	m_strMiCmd = "exec-finish";
	
	// Required by the CMICmdFactory when registering *this command
	m_pSelfCreatorFn = &CMICmdCmdExecFinish::CreateSelf;
}

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecFinish destructor.
// Type:	Overrideable.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecFinish::~CMICmdCmdExecFinish( void )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The parses the command line options 
//			arguments to extract values for each of those arguments.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecFinish::ParseArgs( void )
{
	bool bOk = m_setCmdArgs.Add( *(new CMICmdArgValOptionLong( m_constStrArgThread, true, true, CMICmdArgValListBase::eArgValType_Number, 1 ) ) );
	bOk = bOk && m_setCmdArgs.Add( *(new CMICmdArgValOptionLong( m_constStrArgFrame, false, false, CMICmdArgValListBase::eArgValType_Number, 1 ) ) );
	return (bOk && ParseValidateCmdOptions() );
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command does work in this function.
//			The command is likely to communicate with the LLDB SBDebugger in here.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecFinish::Execute( void )
{
	CMICMDBASE_GETOPTION( pArgThread, OptionLong, m_constStrArgThread );

	// Retrieve the --thread option's thread ID (only 1)
	MIuint64 nThreadId = UINT64_MAX;
	if( !pArgThread->GetExpectedOption< CMICmdArgValNumber, MIuint64 >( nThreadId ) )
	{
		SetError( CMIUtilString::Format( MIRSRC( IDS_CMD_ERR_OPTION_NOT_FOUND ), m_cmdData.strMiCmd.c_str(), m_constStrArgThread.c_str() ) );
		return MIstatus::failure;
	}
	
	CMICmnLLDBDebugSessionInfo & rSessionInfo( CMICmnLLDBDebugSessionInfo::Instance() );
	lldb::SBDebugger & rDebugger = rSessionInfo.m_rLldbDebugger;
	CMIUtilString strCmd( "thread step-out" );
	if( nThreadId != UINT64_MAX )
		strCmd += CMIUtilString::Format( " %llu", nThreadId );
	rDebugger.GetCommandInterpreter().HandleCommand( strCmd.c_str(), m_lldbResult, false );

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command prepares a MI Record Result
//			for the work carried out in the Execute().
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecFinish::Acknowledge( void )
{
	if( m_lldbResult.GetErrorSize() > 0 )
	{
		const MIchar * pLldbErr = m_lldbResult.GetError();  MIunused( pLldbErr );
		const CMICmnMIValueConst miValueConst( m_lldbResult.GetError() );
		const CMICmnMIValueResult miValueResult( "message", miValueConst );
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Error, miValueResult );
		m_miResultRecord = miRecordResult;
	}
	else
	{
		const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Running );
		m_miResultRecord = miRecordResult;
	}

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Required by the CMICmdFactory when registering *this command. The factory
//			calls this function to create an instance of *this command.
// Type:	Static method.
// Args:	None.
// Return:	CMICmdBase * - Pointer to a new command.
// Throws:	None.
//--
CMICmdBase * CMICmdCmdExecFinish::CreateSelf( void )
{
	return new CMICmdCmdExecFinish();
}

//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------
//---------------------------------------------------------------------------------------

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecInterrupt constructor.
// Type:	Method.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecInterrupt::CMICmdCmdExecInterrupt( void )
{
	// Command factory matches this name with that received from the stdin stream
	m_strMiCmd = "exec-interrupt";
	
	// Required by the CMICmdFactory when registering *this command
	m_pSelfCreatorFn = &CMICmdCmdExecInterrupt::CreateSelf;
}

//++ ------------------------------------------------------------------------------------
// Details:	CMICmdCmdExecInterrupt destructor.
// Type:	Overrideable.
// Args:	None.
// Return:	None.
// Throws:	None.
//--
CMICmdCmdExecInterrupt::~CMICmdCmdExecInterrupt( void )
{
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command does work in this function.
//			The command is likely to communicate with the LLDB SBDebugger in here.
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecInterrupt::Execute( void )
{
	CMICmnLLDBDebugSessionInfo & rSessionInfo( CMICmnLLDBDebugSessionInfo::Instance() );
	lldb::SBDebugger & rDebugger = rSessionInfo.m_rLldbDebugger;
	CMIUtilString strCmd( "process interrupt" );
	const lldb::ReturnStatus status = rDebugger.GetCommandInterpreter().HandleCommand( strCmd.c_str(), m_lldbResult, false ); MIunused( status );

	// CODETAG_DEBUG_SESSION_RUNNING_PROG_RECEIVED_SIGINT_PAUSE_PROGRAM
	if( !CMIDriver::Instance().SetDriverStateRunningNotDebugging() )
	{
		const CMIUtilString & rErrMsg( CMIDriver::Instance().GetErrorDescription() );
		SetErrorDescription( CMIUtilString::Format( MIRSRC( IDS_CMD_ERR_SET_NEW_DRIVER_STATE ), strCmd.c_str(), rErrMsg.c_str() ) );
		return MIstatus::failure;
	}

	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	The invoker requires this function. The command prepares a MI Record Result
//			for the work carried out in the Execute().
// Type:	Overridden.
// Args:	None.
// Return:	MIstatus::success - Functional succeeded.
//			MIstatus::failure - Functional failed.
// Throws:	None.
//--
bool CMICmdCmdExecInterrupt::Acknowledge( void )
{
	if( m_lldbResult.GetErrorSize() > 0 )
    {
        const CMICmnMIValueConst miValueConst( m_lldbResult.GetError() );
        const CMICmnMIValueResult miValueResult( "message", miValueConst );
        const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Error, miValueResult );
        m_miResultRecord = miRecordResult;
    }
    else
    {
        const CMICmnMIResultRecord miRecordResult( m_cmdData.strMiCmdToken, CMICmnMIResultRecord::eResultClass_Done );
		m_miResultRecord = miRecordResult;
	}
	
	return MIstatus::success;
}

//++ ------------------------------------------------------------------------------------
// Details:	Required by the CMICmdFactory when registering *this command. The factory
//			calls this function to create an instance of *this command.
// Type:	Static method.
// Args:	None.
// Return:	CMICmdBase * - Pointer to a new command.
// Throws:	None.
//--
CMICmdBase * CMICmdCmdExecInterrupt::CreateSelf( void )
{
	return new CMICmdCmdExecInterrupt();
}

