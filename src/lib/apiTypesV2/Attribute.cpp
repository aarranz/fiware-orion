/*
*
* Copyright 2015 Telefonica Investigacion y Desarrollo, S.A.U
*
* This file is part of Orion Context Broker.
*
* Orion Context Broker is free software: you can redistribute it and/or
* modify it under the terms of the GNU Affero General Public License as
* published by the Free Software Foundation, either version 3 of the
* License, or (at your option) any later version.
*
* Orion Context Broker is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero
* General Public License for more details.
*
* You should have received a copy of the GNU Affero General Public License
* along with Orion Context Broker. If not, see http://www.gnu.org/licenses/.
*
* For those usages not covered by this license please contact with
* iot_support at tid dot es
*
* Author: Orion dev team
*/
#include <string>
#include <vector>

#include "rapidjson/reader.h"

#include "common/tag.h"
#include "common/errorMessages.h"
#include "common/RenderFormat.h"
#include "common/string.h"
#include "ngsi10/QueryContextResponse.h"
#include "apiTypesV2/Attribute.h"



/* ****************************************************************************
*
* Attribute::render -
*/
void Attribute::render
(
  rapidjson::Writer<rapidjson::StringBuffer>& writer,
  ApiVersion          apiVersion,          // in parameter (pass-through)
  bool                acceptedTextPlain,   // in parameter (pass-through)
  bool                acceptedJson,        // in parameter (pass-through)
  MimeType            outFormatSelection,  // in parameter (pass-through)
  MimeType*           outMimeTypeP,        // out parameter (pass-through)
  HttpStatusCode*     scP,                 // out parameter (pass-through)
  bool                keyValues,           // in parameter
  const std::string&  metadataList,        // in parameter
  RequestType         requestType          // in parameter
)
{
  RenderFormat  renderFormat = (keyValues == true)? NGSI_V2_KEYVALUES : NGSI_V2_NORMALIZED;

  if (pcontextAttribute)
  {
    if (requestType == EntityAttributeValueRequest)
    {
      std::string json = pcontextAttribute->toJsonAsValue(apiVersion,
                                                         acceptedTextPlain,
                                                         acceptedJson,
                                                         outFormatSelection,
                                                         outMimeTypeP,
                                                         scP);
      rapidjson::Reader reader;
      rapidjson::StringStream ss(json.c_str());
      reader.Parse(ss, writer);
    }
    else
    {
      std::vector<std::string> metadataFilter;

      if (metadataList != "")
      {
        stringSplit(metadataList, ',', metadataFilter);
      }

      writer.StartObject();

      pcontextAttribute->toJson(writer, renderFormat, metadataFilter, requestType);

      writer.EndObject();
    }
  }

  oe.toJson(&writer);
}




/* ****************************************************************************
*
* Attribute::fill -
*
* CAUTION
*   The Query should be for an indvidual entity
*
*/
void Attribute::fill(QueryContextResponse* qcrsP, std::string attrName)
{
  if (qcrsP->errorCode.code == SccContextElementNotFound)
  {
    oe.fill(SccContextElementNotFound, ERROR_DESC_NOT_FOUND_ENTITY, ERROR_NOT_FOUND);
  }
  else if (qcrsP->errorCode.code != SccOk)
  {
    //
    // any other error distinct from Not Found
    //
    oe.fill(qcrsP->errorCode.code, qcrsP->errorCode.details, qcrsP->errorCode.reasonPhrase);
  }
  else if (qcrsP->contextElementResponseVector.size() > 1)  // qcrsP->errorCode.code == SccOk
  {
    //
    // If there are more than one entity, we return an error
    //
    oe.fill(SccConflict, ERROR_DESC_TOO_MANY_ENTITIES, ERROR_TOO_MANY);
  }
  else
  {
    pcontextAttribute = NULL;
    // Look for the attribute by name

    ContextElementResponse* cerP = qcrsP->contextElementResponseVector[0];

    for (std::size_t i = 0; i < cerP->contextElement.contextAttributeVector.size(); ++i)
    {
      if (cerP->contextElement.contextAttributeVector[i]->name == attrName)
      {
        pcontextAttribute = cerP->contextElement.contextAttributeVector[i];
        break;
      }
    }

    if (pcontextAttribute == NULL)
    {
      oe.fill(SccContextElementNotFound, ERROR_DESC_NOT_FOUND_ATTRIBUTE, ERROR_NOT_FOUND);
    }
  }
}
