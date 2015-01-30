//--------------------------------------------------------------------------------------------------
/**
 * Implementation of the legato::Exception class.
 *
 * Copyright (C) Sierra Wireless Inc. Use of this work is subject to license.
 */
//--------------------------------------------------------------------------------------------------

#include "LegatoObjectModel.h"


//--------------------------------------------------------------------------------------------------
/**
 *  Constructor
 */
//--------------------------------------------------------------------------------------------------
legato::Exception::Exception
(
    const std::string& message  /// Description of the problem that was encountered.
)
//--------------------------------------------------------------------------------------------------
:   std::runtime_error(message)
//--------------------------------------------------------------------------------------------------
{
}



//--------------------------------------------------------------------------------------------------
/**
 *  Constructor
 */
//--------------------------------------------------------------------------------------------------
legato::DependencyException::DependencyException
(
    const std::string& message  /// Description of the problem that was encountered.
)
//--------------------------------------------------------------------------------------------------
:   legato::Exception(message)
//--------------------------------------------------------------------------------------------------
{
}