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

#ifndef LOGHANDLERLIST_H
#define LOGHANDLERLIST_H

class LogHandler;
#include <ndb_global.h>

/**
 * Provides a simple linked list of log handlers.
 *
 * @see LogHandler
 * @version #@ $Id: LogHandlerList.hpp,v 1.1 2006/01/28 00:10:24 kurt Exp $
 */
class LogHandlerList
{
public:
  /**
   * Default Constructor.
   */
  LogHandlerList();

  /**
   * Destructor.
   */
  ~LogHandlerList();

  /**
   * Adds a new log handler.
   *
   * @param pNewHandler log handler.
   */
  void add(LogHandler* pNewHandler);

  /**
   * Removes a log handler from the list and call its destructor.
   *
   * @param pRemoveHandler the handler to remove
   */
  bool remove(LogHandler* pRemoveHandler);

  /**
   * Removes all log handlers.
   */
  void removeAll();

  /**
   * Returns the next log handler in the list. 
   * returns a log handler or NULL.
   */
  LogHandler* next();

  /**
   * Returns the size of the list.
   */ 
  int size() const;
private:
  /** List node */
  struct LogHandlerNode
  {
    LogHandlerNode* pPrev;
    LogHandlerNode* pNext;    
    LogHandler* pHandler;
  };

  LogHandlerNode* next(LogHandlerNode* pNode);
  LogHandlerNode* prev(LogHandlerNode* pNode);

  void removeNode(LogHandlerNode* pNode);

  int m_size;

  LogHandlerNode* m_pHeadNode;
  LogHandlerNode* m_pTailNode;
  LogHandlerNode* m_pCurrNode;
};

#endif


