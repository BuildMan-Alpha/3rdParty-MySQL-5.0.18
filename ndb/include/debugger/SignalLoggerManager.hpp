/* Copyright (C) 2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

//****************************************************************************
//
// .NAME
//      SignalLoggerManager - Handle signal loggers
//
//****************************************************************************
#ifndef SignalLoggerManager_H
#define SignalLoggerManager_H


#include <kernel_types.h>
#include <BlockNumbers.h>
#include <TransporterDefinitions.hpp>

class SignalLoggerManager
{
public:
  SignalLoggerManager();
  virtual ~SignalLoggerManager();

  /**
   * Sets output
   * @Returns old output stream
   */
  FILE * setOutputStream(FILE * output);
  
  /**
   * Gets current output
   */
  FILE * getOutputStream() const;

  void flushSignalLog();
  
  /**
   * For direct signals
   * @See also SimulatedBlock EXECUTE_DIRECT
   */   
  void executeDirect(const SignalHeader&, 
		     Uint8 prio, const Uint32 * theData, Uint32 node);
  
  /**
   * For input signals
   */
  void executeSignal(const SignalHeader&, Uint8 prio,
                     const Uint32 * theData, Uint32 node,
                     const SegmentedSectionPtr ptr[3], Uint32 secs);

  void executeSignal(const SignalHeader&, Uint8 prio,
                     const Uint32 * theData, Uint32 node,
                     const LinearSectionPtr ptr[3], Uint32 secs);

  /**
   * For output signals
   */
  void sendSignal(const SignalHeader&, Uint8 prio, 
		  const Uint32 * theData, Uint32 node,
                  const SegmentedSectionPtr ptr[3], Uint32 secs);

  void sendSignal(const SignalHeader&, Uint8 prio, 
		  const Uint32 * theData, Uint32 node,
                  const LinearSectionPtr ptr[3], Uint32 secs);
  
  /**
   * For output signals
   */
  void sendSignalWithDelay(Uint32 delayInMilliSeconds, 
			   const SignalHeader&, 
			   Uint8 prio, const Uint32 * data, Uint32 node,
                           const SegmentedSectionPtr ptr[3], Uint32 secs);
  
  /**
   * Generic messages in the signal log
   */
  void log(BlockNumber bno, const char * msg, ...);
  
  /**
   * LogModes
   */
  enum LogMode {
    LogOff   = 0,
    LogIn    = 1,
    LogOut   = 2,
    LogInOut = 3
  };

  /**
   * Returns no of loggers affected
   */
  int log(LogMode logMode, const char * params);
  int logOn(bool allBlocks, BlockNumber bno, LogMode logMode);
  int logOff(bool allBlocks, BlockNumber bno, LogMode logMode);
  int logToggle(bool allBlocks, BlockNumber bno, LogMode logMode);
  
  void setTrace(unsigned long trace);   
  unsigned long getTrace() const;

  void setOwnNodeId(int nodeId);
  void setLogDistributed(bool val);

  /**
   * Print header
   */
  static void printSignalHeader(FILE * output, 
				const SignalHeader & sh,
				Uint8 prio, 
				Uint32 node,
				bool printReceiversSignalId);
  
  /**
   * Function for printing the Signal Data
   */
  static void printSignalData(FILE * out, 
			      const SignalHeader & sh, const Uint32 *);

  /**
   * Print linear section.
   */
  static void printLinearSection(FILE * output,
                                 const SignalHeader & sh,
                                 const LinearSectionPtr ptr[3],
                                 unsigned i);

  /**
   * Print segmented section.
   */
  static void printSegmentedSection(FILE * output,
                                    const SignalHeader & sh,
                                    const SegmentedSectionPtr ptr[3],
                                    unsigned i);

  /**
   * Print data word in hex.  Adds line break before the word
   * when pos > 0 && pos % 7 == 0.  Increments pos.
   */
  static void printDataWord(FILE * output, Uint32 & pos, const Uint32 data);

private:
  bool m_logDistributed;
  Uint32 m_ownNodeId;

  FILE * outputStream;
  int log(int cmd, BlockNumber bno, LogMode logMode);
  
  Uint32        traceId;
  Uint8         logModes[NO_OF_BLOCKS];
  
  inline bool
  logMatch(BlockNumber bno, LogMode mask)
  {
    // avoid addressing outside logModes
    return
      bno < MIN_BLOCK_NO || bno > MAX_BLOCK_NO ||
      (logModes[bno-MIN_BLOCK_NO] & mask);
  }
};

#endif // SignalLoggerManager_H

