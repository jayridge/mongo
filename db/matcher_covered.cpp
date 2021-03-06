// matcher_covered.cpp

/* Matcher is our boolean expression evaluator for "where" clauses */

/**
*    Copyright (C) 2008 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "stdafx.h"
#include "matcher.h"
#include "../util/goodies.h"
#include "../util/unittest.h"
#include "diskloc.h"
#include "../scripting/engine.h"
#include "db.h"
#include "client.h"

#include "pdfile.h"

namespace mongo {

    CoveredIndexMatcher::CoveredIndexMatcher(const BSONObj &jsobj, const BSONObj &indexKeyPattern) :
        _keyMatcher(jsobj.filterFieldsUndotted(indexKeyPattern, true), 
        indexKeyPattern),
        _docMatcher(jsobj) 
    {
        _needRecord = ! ( 
                         _docMatcher.keyMatch() && 
                         _keyMatcher.jsobj.nFields() == _docMatcher.jsobj.nFields() &&
                         ! _keyMatcher.hasType( BSONObj::opEXISTS )
                          );

    }

    bool CoveredIndexMatcher::matchesCurrent( Cursor * cursor , MatchDetails * details ){
        return matches( cursor->currKey() , cursor->currLoc() , details );
    }
    
    bool CoveredIndexMatcher::matches(const BSONObj &key, const DiskLoc &recLoc , MatchDetails * details ) {
        if ( details )
            details->reset();
        
        if ( _keyMatcher.keyMatch() ) {
            if ( !_keyMatcher.matches(key, details ) ){
                return false;
            }
        }
        
        if ( ! _needRecord ){
            return true;
        }

        if ( details )
            details->loadedObject = true;

        return _docMatcher.matches(recLoc.rec() , details );
    }
    

}
