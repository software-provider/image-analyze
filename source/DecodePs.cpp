// JPEGsnoop - JPEG Image Decoder & Analysis Utility
// Copyright (C) 2017 - Calvin Hass
// http://www.impulseadventure.com/photo/jpeg-snoop.html
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//



#include "stdafx.h"

#include "DecodePs.h"
#include "snoop.h"
#include "JPEGsnoop.h" // for m_pAppConfig get

#include "WindowBuf.h"


// Forward declarations
struct tsIptcField	asIptcFields[];
struct tsBimRecord	asBimRecords[];
struct tsBimEnum	asBimEnums[];

CDecodePs::CDecodePs(CwindowBuf* pWBuf,CDocLog* pLog)
{
	// Save copies of class pointers
	m_pWBuf = pWBuf;
	m_pLog = pLog;

	// HACK
	// Only select a single layer to decode into the DIB
	// FIXME: Need to allow user control over this setting
	m_bDisplayLayer = false;
	m_nDisplayLayerInd = 0;
	m_bDisplayImage = true;

	// Ideally this would be passed by constructor, but simply access
	// directly for now.
	CJPEGsnoopApp*	pApp;
	pApp = (CJPEGsnoopApp*)AfxGetApp();
    m_pAppConfig = pApp->m_pAppConfig;

	Reset();
}

void CDecodePs::Reset()
{
	m_bPsd = false;
	m_nQualitySaveAs = 0;
	m_nQualitySaveForWeb = 0;
	m_bAbort = false;
}


CDecodePs::~CDecodePs(void)
{
}


// Provide a short-hand alias for the m_pWBuf buffer
// INPUT:
// - offset					File offset to read from
// - bClean					Forcibly disables any redirection to Fake DHT table
//
// POST:
// - m_pLog
//
// RETURN:
// - Byte from file
//
BYTE CDecodePs::Buf(unsigned long offset,bool bClean=false)
{
	return m_pWBuf->Buf(offset,bClean);
}


// Determine if the file is a Photoshop PSD
// If so, parse the headers. Generally want to start at start of file (nPos=0).
bool CDecodePs::DecodePsd(unsigned long nPos,CDIB* pDibTemp,unsigned &nWidth,unsigned &nHeight)
{
	CString			strTmp;

	m_bPsd = false;

	CString		strSig;
	unsigned	nVer;

	strSig = m_pWBuf->BufReadStrn(nPos,4); nPos+=4;
	nVer = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	if ((strSig == _T("8BPS")) && (nVer == 1)) {
		m_bPsd = true;
		m_pLog->AddLine(_T(""));
		m_pLog->AddLineHdr(_T("*** Photoshop PSD File Decoding ***"));
		m_pLog->AddLine(_T("Decoding Photoshop format..."));
		m_pLog->AddLine(_T(""));
	} else {
		return false;
	}

	// Rewind back to header (signature)
	nPos-=6;

	bool bDecOk = true;

	tsImageInfo		sImageInfo;

	PhotoshopParseFileHeader(nPos,3,&sImageInfo);
	nWidth = sImageInfo.nImageWidth;
	nHeight = sImageInfo.nImageHeight;

	PhotoshopParseColorModeSection(nPos,3);
	if (bDecOk)
		bDecOk &= PhotoshopParseImageResourcesSection(nPos,3);

	if (bDecOk)
		bDecOk &= PhotoshopParseLayerMaskInfo(nPos,3,pDibTemp);
	if (bDecOk) {
		unsigned char*	pDibBits = NULL;

#ifdef PS_IMG_DEC_EN
		if ((pDibTemp) && (m_bDisplayImage)) {
			// Allocate the device-independent bitmap (DIB)
			// - Although we are creating a 32-bit DIB, it should also
			//   work to run in 16- and 8-bit modes

			// Start by killing old DIB since we might be changing its dimensions
			pDibTemp->Kill();

			// Allocate the new DIB
			bool bDibOk = pDibTemp->CreateDIB(nWidth,nHeight,32);
			// Should only return false if already exists or failed to allocate
			if (bDibOk) {
				// Fetch the actual pixel array
				pDibBits = (unsigned char*) ( pDibTemp->GetDIBBitArray() );
			}			
		}
#endif

		bDecOk &= PhotoshopParseImageData(nPos,3,&sImageInfo,pDibBits);
		PhotoshopParseReportFldOffset(3,_T("Image data decode complete:"),nPos);
	}
	PhotoshopParseReportNote(3,_T(""));

	if (bDecOk) {
	} else {
		m_pLog->AddLineErr(_T("ERROR: There was a problem during decode. Aborting."));
		return false;
	}
	return true;

}


// Locate a field ID in the constant 8BIM / Image Resource array
// 
// INPUT:
// - nBimId				= Image Resource ID (16-bit unsigned)
// OUTPUT:
// - nFldInd			= Index into asBimRecords[] array
// RETURN:
// - Returns true if ID was found in array
// NOTE:
// - The constant struct array depends on BIM_T_END as a terminator for the list
//
bool CDecodePs::FindBimRecord(unsigned nBimId,unsigned &nFldInd)
{
	unsigned	nInd=0;
	bool		bDone=false;
	bool		bFound=false;
	while (!bDone) {
		if (asBimRecords[nInd].eBimType == BIM_T_END) {
			bDone = true;
		} else {
			// Support detection of code ranges
			unsigned nCodeStart = asBimRecords[nInd].nCode;
			unsigned nCodeEnd = asBimRecords[nInd].nCodeEnd;
			if ((nCodeEnd == 0) && (nCodeStart == nBimId)) {
				// Match with single code (ie. not ranged entry)
				bDone = true;
				bFound = true;
			} else if ((nCodeEnd != 0) && (nCodeStart <= nBimId) && (nCodeEnd >= nBimId)) {
				// Match with code in ranged entry
				bDone = true;
				bFound = true;
			} else {
				nInd++;
			}

		}
	}
	if (bFound) {
		nFldInd = nInd;
	}
	return bFound;
}

// Search the constant array of IPTC fields for the Record:DataSet index
//
// OUTPUT:
// - nFldInd  = The index into the array of the located entry (if found)
// RETURN
// - Successfully found the Record:DataSet value
//
bool CDecodePs::LookupIptcField(unsigned nRecord,unsigned nDataSet,unsigned &nFldInd)
{
	unsigned	nInd=0;
	bool		bDone=false;
	bool		bFound=false;
	while (!bDone) {
		if (asIptcFields[nInd].eFldType == IPTC_T_END) {
			bDone = true;
		} else {
			if ((asIptcFields[nInd].nRecord == nRecord) && (asIptcFields[nInd].nDataSet == nDataSet)) {
				bDone = true;
				bFound = true;
			} else {
				nInd++;
			}
		}
	}
	if (bFound) {
		nFldInd = nInd;
	}
	return bFound;
}



// Generate the custom-formatted string representing the IPTC field name and value
//
// INPUT:
// - eIptcType				= Enumeration representing the IPTC field type
// - nFldCnt				= The number of elements in this field to report
// - nPos					= The starting file offset for this field
// RETURN:
// - The IPTC formattted string
// NOTE:
// - The IPTC field type is used to determine how to represent the input values
//
CString	CDecodePs::DecodeIptcValue(teIptcType eIptcType,unsigned nFldCnt,unsigned long nPos)
{
	//unsigned	nFldInd = 0;
	unsigned	nInd;
	unsigned	nVal;
	CString		strField = _T("");
	CString		strByte = _T("");
	CString		strVal = _T("");
	switch (eIptcType) {
		case IPTC_T_NUM:
		case IPTC_T_NUM1:
		case IPTC_T_NUM2:
			nVal = m_pWBuf->BufX(nPos,nFldCnt,PS_BSWAP); nPos+=nFldCnt;
			nPos += nFldCnt;
			strVal.Format(_T("%u"),nVal);
			break;
		case IPTC_T_HEX:
			strVal = _T("[");
			for (nInd=0;nInd<nFldCnt;nInd++) {
				strByte.Format(_T("0x%02X "),Buf(nPos++));
				strVal += strByte;
			}
			strVal += _T("]");
			break;
		case IPTC_T_STR:
			strVal = _T("\"");
			for (nInd=0;nInd<nFldCnt;nInd++) {
				strByte.Format(_T("%c"),Buf(nPos++));
				strVal += strByte;
			}
			strVal += _T("\"");
			break;
		case IPTC_T_UNK:
			strVal = _T("???");
		default:
			break;
	}
	return strVal;
}


// Decode the IPTC metadata segment
// INPUT:
//	nLen	: Length of the 8BIM:IPTC resource data
void CDecodePs::DecodeIptc(unsigned long &nPos,unsigned nLen,unsigned nIndent)
{
	CString			strIndent;
	CString			strTmp;
	CString			strIptcTypeName;
	CString			strIptcField;
	CString			strIptcVal;
	CString			strByte;
	unsigned long	nPosStart;
	bool			bDone;
	unsigned		nTagMarker,nRecordNumber,nDataSetNumber,nDataFieldCnt;

	strIndent = PhotoshopParseIndent(nIndent);
	nPosStart = nPos;
	bDone = true;
	if (nPos <= nPosStart+nLen) {
		// TODO: Should probably check to see if we have at least 5 bytes?
		bDone = false;
	}
	while (!bDone) {
		nTagMarker = Buf(nPos+0);
		nRecordNumber = Buf(nPos+1);
		nDataSetNumber = Buf(nPos+2);
		nDataFieldCnt = Buf(nPos+3)*256 + Buf(nPos+4);
		nPos+=5;

		if (nTagMarker == 0x1C) {
			unsigned	nFldInd;
			teIptcType	eIptcType;
			if (LookupIptcField(nRecordNumber,nDataSetNumber,nFldInd)) {
				strIptcField.Format(_T("%-35s"),(LPCTSTR)asIptcFields[nFldInd].strFldName);
				eIptcType = asIptcFields[nFldInd].eFldType;
			} else {
				strIptcField.Format(_T("%-35s"),_T("?"));
				eIptcType = IPTC_T_UNK;
			}

			strIptcVal = DecodeIptcValue(eIptcType,nDataFieldCnt,nPos);
			strTmp.Format(_T("%sIPTC [%03u:%03u] %s = %s"),
				(LPCTSTR)strIndent,nRecordNumber,nDataSetNumber,(LPCTSTR)strIptcField,(LPCTSTR)strIptcVal);
			m_pLog->AddLine(strTmp);
			nPos += nDataFieldCnt;
		} else {
			// Unknown Tag Marker
			// I have seen at least one JPEG file that had an IPTC block with all zeros.
			// In this example, the TagMarker check for 0x1C would fail.
			// Since we don't know how to parse it, abort now.
			strTmp.Format(_T("ERROR: Unknown IPTC TagMarker [0x%02X] @ 0x%08X. Skipping parsing."),nTagMarker,nPos-5);
			m_pLog->AddLineErr(strTmp);

#ifdef DEBUG_LOG
			CString	strDebug;
			strDebug.Format(_T("## File=[%-100s] Block=[%-10s] Error=[%s]\n"),(LPCTSTR)m_pAppConfig->strCurFname,
				_T("PsDecode"),(LPCTSTR)strTmp);
			OutputDebugString(strDebug);
#endif

			// Adjust length to end
			nPos = nPosStart + nLen;

			// Now abort
			bDone = true;
		}

		if (nPos >= nPosStart+nLen) {
			bDone = true;
		}
	}
}


// Create indent string
// - Returns a sequence of spaces according to the indent level
CString CDecodePs::PhotoshopParseIndent(unsigned nIndent)
{
	CString	strIndent = _T("");
	for (unsigned nInd=0;nInd<nIndent;nInd++) {
		strIndent += _T("  ");
	}
	return strIndent;
}

// Get ASCII string according to Photoshop format
// - String length prefix
// - If length is 0 then fixed 4-character string
// - Otherwise it is defined length string (no terminator required)
CString CDecodePs::PhotoshopParseGetLStrAsc(unsigned long &nPos)
{
	unsigned	nStrLen;
	CString		strVal = _T("");
	nStrLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	if (nStrLen!=0) {
		strVal = m_pWBuf->BufReadStrn(nPos,nStrLen);
		nPos += nStrLen;
	} else {
		// 4 byte string
		strVal.Format(_T("%c%c%c%c"),Buf(nPos+0),Buf(nPos+1),Buf(nPos+2),Buf(nPos+3));
		nPos+=4;
	}
	return strVal;
}

// Read a UNICODE string starting at nPos according to Photoshop Unicode string format
// - Start with 4 byte length
// - Follow with UNICODE string
// OUTPUT:
// - The byte offset to advance the file pointer
// RETURN:
// - Unicode string
CString CDecodePs::PhotoshopParseGetBimLStrUni(unsigned long nPos,unsigned &nPosOffset)
{
	CString		strVal;
	unsigned	nStrLenActual,nStrLenTrunc;
	BYTE		anStrBuf[(PS_MAX_UNICODE_STRLEN+1)*2];
	wchar_t		acStrBuf[(PS_MAX_UNICODE_STRLEN+1)];
	BYTE		nChVal;
	nPosOffset = 0;
	nStrLenActual = m_pWBuf->BufX(nPos+nPosOffset,4,PS_BSWAP);
	nPosOffset+=4;
	nStrLenTrunc = nStrLenActual;

	// Initialize return
	strVal = _T("");


	if (nStrLenTrunc>0) {
		// Read unicode bytes into byte array
		// Truncate the string, leaving room for terminator
		if (nStrLenTrunc>PS_MAX_UNICODE_STRLEN) {
			nStrLenTrunc = PS_MAX_UNICODE_STRLEN;
		}
		for (unsigned nInd=0;nInd<nStrLenTrunc;nInd++) {
			// Reverse the order of the bytes
			nChVal = Buf(nPos+nPosOffset+(nInd*2)+0);
			anStrBuf[(nInd*2)+1] = nChVal;
			nChVal = Buf(nPos+nPosOffset+(nInd*2)+1);
			anStrBuf[(nInd*2)+0] = nChVal;
		}
		// TODO: Replace with call to ByteStr2Unicode()
		// Ensure it is terminated
		anStrBuf[nStrLenTrunc*2+0] = 0;
		anStrBuf[nStrLenTrunc*2+1] = 0;
		// Copy into unicode string
		// Ensure that it is terminated first!
		lstrcpyW(acStrBuf,(LPCWSTR)anStrBuf);
		// Copy into CString
		strVal = acStrBuf;
	}
	// Update the file position offset
	nPosOffset += nStrLenActual*2;
	// Return
	return strVal;
}


// Display a formatted note line
// - Apply the current indent level (nIndent)
void CDecodePs::PhotoshopParseReportNote(unsigned nIndent,CString strNote)
{
	CString		strIndent;
	CString		strLine;

	strIndent = PhotoshopParseIndent(nIndent);
	strLine.Format(_T("%s%-50s"),(LPCTSTR)strIndent,(LPCTSTR)strNote);
	m_pLog->AddLine(strLine);
}


// Display a formatted numeric field with optional units string
// - Report the value with the field name (strField) and current indent level (nIndent)
void CDecodePs::PhotoshopParseReportFldNum(unsigned nIndent,CString strField,unsigned nVal,CString strUnits)
{
	CString		strIndent;
	CString		strVal;
	CString		strLine;

	strIndent = PhotoshopParseIndent(nIndent);
	strVal.Format(_T("%u"),nVal);
	strLine.Format(_T("%s%-50s = %s %s"),(LPCTSTR)strIndent,(LPCTSTR)strField,(LPCTSTR)strVal,(LPCTSTR)strUnits);
	m_pLog->AddLine(strLine);
}

// Display a formatted boolean field
// - Report the value with the field name (strField) and current indent level (nIndent)
void CDecodePs::PhotoshopParseReportFldBool(unsigned nIndent,CString strField,unsigned nVal)
{
	CString		strIndent;
	CString		strVal;
	CString		strLine;

	strIndent = PhotoshopParseIndent(nIndent);
	strVal = (nVal!=0)? _T("true") : _T("false");
	strLine.Format(_T("%s%-50s = %s"),(LPCTSTR)strIndent,(LPCTSTR)strField,(LPCTSTR)strVal);
	m_pLog->AddLine(strLine);
}


//
// Display a formatted hexadecimal field
// - Report the value with the field name (strField) and current indent level (nIndent)
// - Depending on the length of the buffer, either report the hex value in-line with
//   the field name or else start a new row and begin a hex dump
// - Also report the ASCII representation alongside the hex dump
//
// INPUT:
// - nIndent		= Current indent level for reporting
// - strField		= Field name
// - nPosStart		= Field byte array file position start
// - nLen			= Field byte array length
//
void CDecodePs::PhotoshopParseReportFldHex(unsigned nIndent,CString strField,unsigned long nPosStart,unsigned nLen)
{
	CString		strIndent;
	unsigned	nByte;
	CString		strPrefix;
	CString		strByteHex;
	CString		strByteAsc;
	CString		strValHex;
	CString		strValAsc;
	CString		strLine;

	strIndent = PhotoshopParseIndent(nIndent);

	if (nLen == 0) {
		// Print out the header row, but no data will be shown
		strLine.Format(_T("%s%-50s = "),(LPCTSTR)strIndent,(LPCTSTR)strField);
		m_pLog->AddLine(strLine);
		// Nothing to report, exit now
		return;
	} else if (nLen <= PS_HEX_MAX_INLINE) {
		// Define prefix for row
		strPrefix.Format(_T("%s%-50s = "),(LPCTSTR)strIndent,(LPCTSTR)strField);
	} else {
		// Print out header row
		strLine.Format(_T("%s%-50s ="),(LPCTSTR)strIndent,(LPCTSTR)strField);
		m_pLog->AddLine(strLine);
		// Define prefix for next row
		strPrefix.Format(_T("%s"),(LPCTSTR)strIndent);
	}


	// Build up the hex string
	// Limit to PS_HEX_TOTAL bytes
	unsigned	nRowOffset = 0;
	bool		bDone = false;
	unsigned	nLenClip;
	nLenClip = min(nLen,PS_HEX_TOTAL);
	strValHex = _T("");
	strValAsc = _T("");
	while (!bDone) {
		// Have we reached the end of the data we wish to display?
		if (nRowOffset>=nLenClip) {
			bDone = true;
		} else {
			// Reset the cumulative hex/ascii value strings
			strValHex = _T("");
			strValAsc = _T("");
			// Build the hex/ascii value strings
			for (unsigned nInd=0;nInd<PS_HEX_MAX_ROW;nInd++) {
				if ((nRowOffset+nInd)<nLenClip) {
					nByte = m_pWBuf->Buf(nPosStart+nRowOffset+nInd);
					strByteHex.Format(_T("%02X "),nByte);
					// Only display printable characters
					if (isprint(nByte)) {
						strByteAsc.Format(_T("%c"),nByte);
					} else {
						strByteAsc = _T(".");
					}
				} else {
					// Pad out to end of row
					strByteHex.Format(_T("   "));
					strByteAsc = _T(" ");
				}
				// Build up the strings
				strValHex += strByteHex;
				strValAsc += strByteAsc;
			}

			// Generate the line with Hex and ASCII representations
			strLine.Format(_T("%s | 0x%s | %s"),(LPCTSTR)strPrefix,(LPCTSTR)strValHex,(LPCTSTR)strValAsc);
			m_pLog->AddLine(strLine);

			// Now increment file offset
			nRowOffset += PS_HEX_MAX_ROW;
		}
	}

	// If we had to clip the display length, then show ellipsis now
	if (nLenClip < nLen) {
		strLine.Format(_T("%s | ..."),(LPCTSTR)strPrefix);
		m_pLog->AddLine(strLine);
	}


}

CString CDecodePs::PhotoshopDispHexWord(unsigned nVal)
{
	CString	strValHex;
	CString	strValAsc;
	unsigned	nByte;
	CString		strByteHex;
	CString		strByteAsc;
	CString		strLine;

	// Reset the cumulative hex/ascii value strings
	strValHex = _T("");
	strValAsc = _T("");
	// Build the hex/ascii value strings
	for (unsigned nInd=0;nInd<=3;nInd++) {
		nByte = (nVal & 0xFF000000) >> 24;
		nVal <<= 8;

		strByteHex.Format(_T("%02X "),nByte);
		// Only display printable characters
		if (isprint(nByte)) {
			strByteAsc.Format(_T("%c"),nByte);
		} else {
			strByteAsc = _T(".");
		}

		// Build up the strings
		strValHex += strByteHex;
		strValAsc += strByteAsc;
	}

	// Generate the line with Hex and ASCII representations
	strLine.Format(_T("0x%s | %s"),(LPCTSTR)strValHex,(LPCTSTR)strValAsc);

	return strLine;
}

// Look up the enumerated constant (nVal) for the specified field (eEnumField)
// - Returns "" if the field wasn't found
CString CDecodePs::PhotoshopParseLookupEnum(teBimEnumField eEnumField,unsigned nVal)
{
	CString		strVal = _T("");
	// Find the enum value
	bool	bDone = false;
	bool	bFound = false;
	unsigned	nEnumInd=0;
	while (!bDone) {
		if (asBimEnums[nEnumInd].eBimEnum == eEnumField) {
			if (asBimEnums[nEnumInd].nVal == nVal) {
				bDone = true;
				bFound = true;
				strVal = asBimEnums[nEnumInd].strVal;
			}
		}
		if (asBimEnums[nEnumInd].eBimEnum == BIM_T_ENUM_END) {
			bDone = true;
		}
		if (!bDone) {
			nEnumInd++;
		}
	}

	// If enum wasn't found, then replace by hex value
	if (!bFound) {
		CString strWord = PhotoshopDispHexWord(nVal);
		strVal.Format(_T("? [%s]"),(LPCTSTR)strWord);
	}
	return strVal;
}

// Display a formatted enumerated type field
// - Look up enumerated constant (nVal) for the given field (eEnumField)
// - Report the value with the field name (strField) and current indent level (nIndent)
void CDecodePs::PhotoshopParseReportFldEnum(unsigned nIndent,CString strField,teBimEnumField eEnumField,unsigned nVal)
{
	CString		strIndent;
	CString		strVal;
	CString		strLine;

	strIndent = PhotoshopParseIndent(nIndent);
	strVal = PhotoshopParseLookupEnum(eEnumField,nVal);
	strLine.Format(_T("%s%-50s = %s"),(LPCTSTR)strIndent,(LPCTSTR)strField,(LPCTSTR)strVal);
	m_pLog->AddLine(strLine);
}

// Display a formatted fixed-point field
// - Convert a 32-bit unsigned integer into Photoshop fixed point
// - Assume fixed point representation is 16-bit integer : 16-bit fraction
// - Report the value with the field name (strField) and current indent level (nIndent)
void CDecodePs::PhotoshopParseReportFldFixPt(unsigned nIndent,CString strField,unsigned nVal,CString strUnits)
{
	CString		strIndent;
	CString		strVal;
	CString		strLine;
	float		fVal;

	fVal = (nVal / (float)65536.0);
	strIndent = PhotoshopParseIndent(nIndent);
	strVal.Format(_T("%.f"),fVal);
	strLine.Format(_T("%s%-50s = %s %s"),(LPCTSTR)strIndent,(LPCTSTR)strField,(LPCTSTR)strVal,(LPCTSTR)strUnits);
	m_pLog->AddLine(strLine);
}

// Display a formatted fixed-point field
// - Convert a 32-bit unsigned integer into Photoshop floating point
// - Report the value with the field name (strField) and current indent level (nIndent)
void CDecodePs::PhotoshopParseReportFldFloatPt(unsigned nIndent,CString strField,unsigned nVal,CString strUnits)
{
	CString		strIndent;
	CString		strVal;
	CString		strLine;
	float		fVal;

	// Convert 4 byte unsigned int to floating-point
	// TODO: Need to check this since Photoshop web spec doesn't
	// indicate how floating points are stored.
	union tUnionTmp
	{
		BYTE		nVal[4];
		float		fVal;
	};
	tUnionTmp	myUnion;	
	// NOTE: Empirically determined the byte order for double representation
	myUnion.nVal[3] = static_cast<BYTE>((nVal & 0xFF000000)>>24);
	myUnion.nVal[2] = static_cast<BYTE>((nVal & 0x00FF0000)>>16);
	myUnion.nVal[1] = static_cast<BYTE>((nVal & 0x0000FF00)>>8);
	myUnion.nVal[0] = static_cast<BYTE>((nVal & 0x000000FF)>>0);
	fVal = myUnion.fVal;

	strIndent = PhotoshopParseIndent(nIndent);
	strVal.Format(_T("%.5f"),fVal);
	strLine.Format(_T("%s%-50s = %s %s"),(LPCTSTR)strIndent,(LPCTSTR)strField,(LPCTSTR)strVal,(LPCTSTR)strUnits);
	m_pLog->AddLine(strLine);
}

// Display a formatted double-precision floating-point field
// - Convert two 32-bit unsigned integers into Photoshop double-precision floating point
// - Report the value with the field name (strField) and current indent level (nIndent)
void CDecodePs::PhotoshopParseReportFldDoublePt(unsigned nIndent,CString strField,unsigned nVal1,unsigned nVal2,CString strUnits)
{
	CString		strIndent;
	CString		strVal;
	CString		strLine;
	double		dVal;

	// Convert 2x4 byte unsigned int to double-point
	// TODO: Need to check this since Photoshop web spec doesn't
	// indicate how double points are stored.
	union tUnionTmp
	{
		BYTE		nVal[8];
		double		dVal;
	};
	tUnionTmp	myUnion;	
	// NOTE: Empirically determined the byte order for double representation
	myUnion.nVal[7] = static_cast<BYTE>((nVal1 & 0xFF000000)>>24);
	myUnion.nVal[6] = static_cast<BYTE>((nVal1 & 0x00FF0000)>>16);
	myUnion.nVal[5] = static_cast<BYTE>((nVal1 & 0x0000FF00)>>8);
	myUnion.nVal[4] = static_cast<BYTE>((nVal1 & 0x000000FF)>>0);
	myUnion.nVal[3] = static_cast<BYTE>((nVal2 & 0xFF000000)>>24);
	myUnion.nVal[2] = static_cast<BYTE>((nVal2 & 0x00FF0000)>>16);
	myUnion.nVal[1] = static_cast<BYTE>((nVal2 & 0x0000FF00)>>8);
	myUnion.nVal[0] = static_cast<BYTE>((nVal2 & 0x000000FF)>>0);
	dVal = myUnion.dVal;

	strIndent = PhotoshopParseIndent(nIndent);
	strVal.Format(_T("%.5f"),dVal);
	strLine.Format(_T("%s%-50s = %s %s"),(LPCTSTR)strIndent,(LPCTSTR)strField,(LPCTSTR)strVal,(LPCTSTR)strUnits);
	m_pLog->AddLine(strLine);
}

// Display a formatted string field
// - Report the value with the field name (strField) and current indent level (nIndent)
void CDecodePs::PhotoshopParseReportFldStr(unsigned nIndent,CString strField,CString strVal)
{
	CString		strIndent;
	CString		strLine;

	strIndent = PhotoshopParseIndent(nIndent);
	strLine.Format(_T("%s%-50s = \"%s\""),(LPCTSTR)strIndent,(LPCTSTR)strField,(LPCTSTR)strVal);
	m_pLog->AddLine(strLine);
}


// Display a formatted file offset field
// - Report the offset with the field name (strField) and current indent level (nIndent)
void CDecodePs::PhotoshopParseReportFldOffset(unsigned nIndent,CString strField,unsigned long nOffset)
{
	CString		strIndent;
	CString		strLine;

	strIndent = PhotoshopParseIndent(nIndent);
	strLine.Format(_T("%s%-50s @ 0x%08X"),(LPCTSTR)strIndent,(LPCTSTR)strField,nOffset);
	m_pLog->AddLine(strLine);
}

// Parse the Photoshop IRB Thumbnail Resource
// - NOTE: Returned nPos doesn't take into account JFIF data
void CDecodePs::PhotoshopParseThumbnailResource(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;

	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Format"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Width of thumbnail"),nVal,_T("pixels"));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Height of thumbnail"),nVal,_T("pixels"));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Widthbytes"),nVal,_T("bytes"));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Total size"),nVal,_T("bytes"));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Size after compression"),nVal,_T("bytes"));
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Bits per pixel"),nVal,_T("bits"));
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Number of planes"),nVal,_T(""));

	PhotoshopParseReportFldOffset(nIndent,_T("JFIF data"),nPos);

	// NOTE: nPos doesn't take into account JFIF data!
}

// Parse the Photoshop IRB Version Info
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseVersionInfo(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;
	unsigned	nPosOffset;

	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Version"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("hasRealMergedData"),nVal,_T(""));
	strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset); nPos+=nPosOffset;
	PhotoshopParseReportFldStr(nIndent,_T("Writer name"),strVal);
	strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset); nPos+=nPosOffset;
	PhotoshopParseReportFldStr(nIndent,_T("Reader name"),strVal);
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("File version"),nVal,_T(""));
}

// Parse the Photoshop IRB Print Scale
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParsePrintScale(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;

	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldEnum(nIndent,_T("Style"),BIM_T_ENUM_PRINT_SCALE_STYLE,nVal);
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldFloatPt(nIndent,_T("X location"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldFloatPt(nIndent,_T("Y location"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldFloatPt(nIndent,_T("Scale"),nVal,_T(""));
}

// Parse the Photoshop IRB Global Angle
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseGlobalAngle(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;

	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Global Angle"),nVal,_T("degrees"));
}

// Parse the Photoshop IRB Global Altitude
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseGlobalAltitude(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;

	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Global Altitude"),nVal,_T(""));
}

// Parse the Photoshop IRB Print Flags
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParsePrintFlags(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;

	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Labels"),nVal);
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Crop marks"),nVal);
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Color bars"),nVal);
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Registration marks"),nVal);
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Negative"),nVal);
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Flip"),nVal);
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Interpolate"),nVal);
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Caption"),nVal);
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Print flags"),nVal);
}

// Parse the Photoshop IRB Print Flags Information
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParsePrintFlagsInfo(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;

	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Version"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Center crop marks"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Reserved"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Bleed width value"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Bleed width scale"),nVal,_T(""));
}

// Parse the Photoshop IRB Copyright Flag
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseCopyrightFlag(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;

	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Copyright flag"),nVal);
}

// Parse the Photoshop IRB Aspect Ratio
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParsePixelAspectRatio(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal,nVal1,nVal2;
	CString		strVal;

	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Version"),nVal,_T(""));
	nVal1 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	nVal2 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldDoublePt(nIndent,_T("X/Y Ratio"),nVal1,nVal2,_T(""));
}

// Parse the Photoshop IRB Document-Specific Seed
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseDocSpecificSeed(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;

	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Base value"),nVal,_T(""));
}

// Parse the Photoshop IRB Grid and Guides
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseGridGuides(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;

	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Version"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Grid Horizontal"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Grid Vertical"),nVal,_T(""));
	unsigned nNumGuides = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Number of Guide Resources"),nNumGuides,_T(""));

	if (nNumGuides>0)
		PhotoshopParseReportNote(nIndent,_T("-----"));
	for (unsigned nInd=0;nInd<nNumGuides;nInd++) {
		strVal.Format(_T("Guide #%u:"),nInd);
		PhotoshopParseReportNote(nIndent,strVal);

		nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent+1,_T("Location"),nVal,_T(""));
		nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
		PhotoshopParseReportFldEnum(nIndent+1,_T("Direction"),BIM_T_ENUM_GRID_GUIDE_DIR,nVal);
	}
	if (nNumGuides>0)
		PhotoshopParseReportNote(nIndent,_T("-----"));
}

// Parse the Photoshop IRB Resolution Info
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseResolutionInfo(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal,nUnit;
	CString		strVal,strUnit;

	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	nUnit = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	strUnit = PhotoshopParseLookupEnum(BIM_T_ENUM_RESOLUTION_INFO_RES_UNIT,nUnit);
	PhotoshopParseReportFldFixPt(nIndent,_T("Horizontal resolution"),nVal,strUnit);
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldEnum(nIndent,_T("Width unit"),BIM_T_ENUM_RESOLUTION_INFO_WIDTH_UNIT,nVal);
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	nUnit = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	strUnit = PhotoshopParseLookupEnum(BIM_T_ENUM_RESOLUTION_INFO_RES_UNIT,nUnit);
	PhotoshopParseReportFldFixPt(nIndent,_T("Vertical resolution"),nVal,strUnit);
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldEnum(nIndent,_T("Height unit"),BIM_T_ENUM_RESOLUTION_INFO_WIDTH_UNIT,nVal);
}

// Parse the Photoshop IRB Layer State Info
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseLayerStateInfo(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;

	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Target layer"),nVal,_T(""));
}

// Parse the Photoshop IRB Layer Group Info
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// - nLen		= Length of IRB entry (to determine number of layers)
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseLayerGroupInfo(unsigned long &nPos,unsigned nIndent,unsigned nLen)
{
	unsigned	nVal;
	CString		strVal;

	// NOTE:
	// According to the Photoshop documentation:
	// - 2 bytes per layer containing a group ID for the dragging groups.
	// - Layers in a group have the same group ID.
	// It was not clear whether there was a separate indication of the number of layers
	// For now, assume we can derive it from the total IRB length
	unsigned	nNumLayers;
	nNumLayers = nLen / 2;

	for (unsigned nInd=0;nInd<nNumLayers;nInd++) {
		strVal.Format(_T("Layer #%u:"),nInd);
		PhotoshopParseReportNote(nIndent,strVal);
		nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent+1,_T("Layer Group"),nVal,_T(""));
	}
}

// Parse the Photoshop IRB Layer Groups Enabled
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// - nLen		= Length of IRB entry (to determine number of layers)
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseLayerGroupEnabled(unsigned long &nPos,unsigned nIndent,unsigned nLen)
{
	unsigned	nVal;
	CString		strVal;

	// NOTE:
	// According to the Photoshop documentation:
	// - 1 byte for each layer in the document, repeated by length of the resource.
	// - NOTE: Layer groups have start and end markers
	// For now, assume we can derive it from the total IRB length
	unsigned	nNumLayers;
	nNumLayers = nLen / 1;

	for (unsigned nInd=0;nInd<nNumLayers;nInd++) {
		strVal.Format(_T("Layer #%u:"),nInd);
		PhotoshopParseReportNote(nIndent,strVal);
		nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent+1,_T("Layer Group Enabled ID"),nVal,_T(""));
	}
}

// Parse the Photoshop IRB Layer Select IDs
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseLayerSelectId(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;

	unsigned nNumLayer = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Num selected"),nNumLayer,_T(""));
	for (unsigned nInd=0;nInd<nNumLayer;nInd++) {
		nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent+1,_T("Layer ID"),nVal,_T(""));
	}
}


// Parse the Photoshop IRB File Header
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseFileHeader(unsigned long &nPos,unsigned nIndent,tsImageInfo* psImageInfo)
{
	ASSERT(psImageInfo);

	unsigned	nVal;
	CString		strVal;

	PhotoshopParseReportNote(nIndent,_T("File Header Section:"));
	nIndent++;

	CString strSig = m_pWBuf->BufReadStrn(nPos,4); nPos+=4;
	PhotoshopParseReportFldStr(nIndent,_T("Signature"),strSig);
	
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Version"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Reserved1"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Reserved2"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Num channels in image"),nVal,_T(""));
	psImageInfo->nNumChans = nVal;
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Image height"),nVal,_T("pixels"));
	psImageInfo->nImageHeight = nVal;
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Image width"),nVal,_T("pixels"));
	psImageInfo->nImageWidth = nVal;
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Depth"),nVal,_T("bits per pixel"));
	psImageInfo->nDepthBpp = nVal;
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldEnum(nIndent,_T("Color mode"),BIM_T_ENUM_FILE_HDR_COL_MODE,nVal);

}

// Parse the Photoshop IRB Color Mode Section (from File Header)
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseColorModeSection(unsigned long &nPos,unsigned nIndent)
{
	CString		strVal;

	PhotoshopParseReportNote(nIndent,_T("Color Mode Data Section:"));
	nIndent++;

	unsigned nColModeLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Length"),nColModeLen,_T(""));
	if (nColModeLen != 0) {
		PhotoshopParseReportFldOffset(nIndent,_T("Color data"),nPos);
	}

	// Skip data
	nPos += nColModeLen;
}

// -------------

// Parse the Photoshop IRB Layer and Mask Info Section (from File Header)
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// - pDibTemp   = DIB for image display
// OUTPUT:
// - nPos		= File position after reading the block
//
bool CDecodePs::PhotoshopParseLayerMaskInfo(unsigned long &nPos,unsigned nIndent,CDIB* pDibTemp)
{
	CString		strVal;
	bool		bDecOk = true;

	PhotoshopParseReportNote(nIndent,_T("Layer and Mask Information Section:"));
	nIndent++;

	unsigned		nLayerMaskLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	unsigned long	nPosStart = nPos;
	unsigned long	nPosEnd = nPosStart + nLayerMaskLen;
	PhotoshopParseReportFldNum(nIndent,_T("Length"),nLayerMaskLen,_T(""));
	if (nLayerMaskLen == 0) {
		return true;
	}
	else {
		if (bDecOk)
			bDecOk &= PhotoshopParseLayerInfo(nPos,nIndent,pDibTemp);
		if (bDecOk)
			bDecOk &= PhotoshopParseGlobalLayerMaskInfo(nPos,nIndent);
		if (bDecOk) {
			//while ((bDecOk) && (nPos < nPosEnd)) {
			//while ((bDecOk) && ((nPos+12) < nPosEnd)) {
			while ((bDecOk) && (nPosStart+nLayerMaskLen-nPos > 12)) {
				bDecOk &= PhotoshopParseAddtlLayerInfo(nPos,nIndent);
			}
		}
	}

	if (bDecOk) {
		// Skip data
		nPos = nPosEnd;
	}

	return bDecOk;
}

// Parse the Photoshop IRB Layer Info Section
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// - pDibTemp   = DIB for image display
// OUTPUT:
// - nPos		= File position after reading the block
//
bool CDecodePs::PhotoshopParseLayerInfo(unsigned long &nPos,unsigned nIndent,CDIB* pDibTemp)
{
	CString		strVal;
	bool		bDecOk = true;

	PhotoshopParseReportNote(nIndent,_T("Layer Info:"));
	nIndent++;

	unsigned nLayerLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Length"),nLayerLen,_T(""));
	if (nLayerLen == 0) {
		return bDecOk;
	}

	// Round up length to multiple of 2
	if (nLayerLen%2 != 0) {
		nLayerLen++;
	}

	// Save file position
	unsigned nPosStart = nPos;

	// According to Adobe, "Layer count" is defined as follows:
	// - If it is a negative number, its absolute value is the number of layers and the
	//   first alpha channel contains the transparency data for the merged result.
	// Therefore, we'll treat it as signed short and take absolute value
	unsigned short	nLayerCountU = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	signed short	nLayerCountS = (signed short)nLayerCountU;
	unsigned		nLayerCount = abs(nLayerCountS);
	PhotoshopParseReportFldNum(nIndent,_T("Layer count"),nLayerCount,_T(""));
	if (nLayerCountU & 0x8000) {
		PhotoshopParseReportNote(nIndent,_T("First alpha channel contains transparency for merged result"));
	}

	tsLayerAllInfo	sLayerAllInfo;
	sLayerAllInfo.nNumLayers = nLayerCount;
	sLayerAllInfo.psLayers = new tsLayerInfo[nLayerCount];
	ASSERT(sLayerAllInfo.psLayers);

	for (unsigned nLayerInd=0;(bDecOk)&&(nLayerInd<nLayerCount);nLayerInd++) {
		// Clear the channel array
		sLayerAllInfo.psLayers[nLayerInd].pnChanLen = NULL;
		CString strTmp;
		strTmp.Format(_T("Layer #%u"),nLayerInd);
		PhotoshopParseReportFldOffset(nIndent,strTmp,nPos);
		if (bDecOk)
			bDecOk &= PhotoshopParseLayerRecord(nPos,nIndent,&sLayerAllInfo.psLayers[nLayerInd]);
	}

	PhotoshopParseReportNote(nIndent,_T("Channel Image Data:"));
	CString		strLine;
	unsigned	nNumChans;
	unsigned	nWidth;
	unsigned	nHeight;
	unsigned	nPosLastLayer=0;
	unsigned	nPosLastChan=0;
	for (unsigned nLayerInd=0;(bDecOk)&&(nLayerInd<nLayerCount);nLayerInd++) {
		nNumChans = sLayerAllInfo.psLayers[nLayerInd].nNumChans;
		nWidth = sLayerAllInfo.psLayers[nLayerInd].nWidth;
		nHeight = sLayerAllInfo.psLayers[nLayerInd].nHeight;


		unsigned char* pDibBits = NULL;
#ifdef PS_IMG_DEC_EN
		if ((pDibTemp) && (m_bDisplayLayer) && (nLayerInd == m_nDisplayLayerInd)) {
			// Allocate the device-independent bitmap (DIB)
			// - Although we are creating a 32-bit DIB, it should also
			//   work to run in 16- and 8-bit modes

			// Start by killing old DIB since we might be changing its dimensions
			pDibTemp->Kill();

			// Allocate the new DIB
			bool bDibOk = pDibTemp->CreateDIB(nWidth,nHeight,32);
			// Should only return false if already exists or failed to allocate
			if (bDibOk) {
				// Fetch the actual pixel array
				pDibBits = (unsigned char*) ( pDibTemp->GetDIBBitArray() );
			}
		}
#endif

		nPosLastLayer = nPos;
		for (unsigned nChanInd=0;(bDecOk)&&(nChanInd<nNumChans);nChanInd++) {
			nPosLastChan = nPos;
			strLine.Format(_T("Layer %3u/%3u, Channel %2u/%2u"),nLayerInd+1,nLayerCount,nChanInd+1,nNumChans);
			//m_pLog->AddLine(strLine);
			PhotoshopParseReportNote(nIndent+1,strLine);
			if (bDecOk) {

				// Fetch the channel ID to determine what RGB channel to map to
				// - Layer 0 (background):           0->R, 1->G, 2->B
				// - Layer n (other):      65535->A, 0->R, 1->G, 2->B
				unsigned	nChanID = sLayerAllInfo.psLayers[nLayerInd].pnChanID[nChanInd];

				// Parse, and optionally paint the DIB
				// NOTE: For uncompressed channels, the width & height generally appear to be zero
				//       According to the spec:
				//       If the compression code is 0, the image data is just the raw image data,
				//       whose size is calculated as (LayerBottom-LayerTop)* (LayerRight-LayerLeft)

				bDecOk &= PhotoshopParseChannelImageData(nPos,nIndent+1,nWidth,nHeight,nChanID,pDibBits);
			}
			strLine.Format(_T("CurPos @ 0x%08X, bDecOk=%u, LastLayer @ 0x%08X, LastChan @ 0x%08X"),nPos,bDecOk,nPosLastLayer,nPosLastChan);
			//m_pLog->AddLine(strLine);
			//PhotoshopParseReportNote(nIndent+1,strLine);
		}
	}

	// Pad out to specified length
	signed nPad;
	nPad = nPosStart + nLayerLen - nPos;
	if (nPad > 0) {
		nPos += nPad;
	}

	// Deallocate
	if (sLayerAllInfo.psLayers) {
		for (unsigned nLayerInd=0;(nLayerInd<sLayerAllInfo.nNumLayers);nLayerInd++) {
			nNumChans = sLayerAllInfo.psLayers[nLayerInd].nNumChans;
			for (unsigned nChanInd=0;(bDecOk)&&(nChanInd<nNumChans);nChanInd++) {
				delete [] sLayerAllInfo.psLayers[nLayerInd].pnChanLen;
				sLayerAllInfo.psLayers[nLayerInd].pnChanLen = NULL;
				delete [] sLayerAllInfo.psLayers[nLayerInd].pnChanID;
				sLayerAllInfo.psLayers[nLayerInd].pnChanID = NULL;
			}
		}
		delete [] sLayerAllInfo.psLayers;
		sLayerAllInfo.psLayers = NULL;
	}



	return bDecOk;
}


// Parse the Photoshop IRB Layer Record
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
bool CDecodePs::PhotoshopParseLayerRecord(unsigned long &nPos,unsigned nIndent,tsLayerInfo* pLayerInfo)
{
	CString		strVal;
	bool		bDecOk = true;

	ASSERT(pLayerInfo);

	PhotoshopParseReportNote(nIndent,_T("Layer Record:"));
	nIndent++;

	unsigned	nRect1,nRect2,nRect3,nRect4;

	nRect1 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	nRect2 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	nRect3 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	nRect4 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Rect Top"),nRect1,_T(""));
	PhotoshopParseReportFldNum(nIndent,_T("Rect Left"),nRect2,_T(""));
	PhotoshopParseReportFldNum(nIndent,_T("Rect Bottom"),nRect3,_T(""));
	PhotoshopParseReportFldNum(nIndent,_T("Rect Right"),nRect4,_T(""));

	pLayerInfo->nHeight = nRect3-nRect1;
	pLayerInfo->nWidth = nRect4-nRect2;
	//nHeight = nRect3-nRect1;

	unsigned nNumChans = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Number of channels"),nNumChans,_T(""));

	//nChans = nNumChans;
	pLayerInfo->nNumChans = nNumChans;

	ASSERT(pLayerInfo->pnChanLen == NULL);
	pLayerInfo->pnChanLen = new unsigned [nNumChans];
	ASSERT(pLayerInfo->pnChanLen);
	pLayerInfo->pnChanID = new unsigned [nNumChans];
	ASSERT(pLayerInfo->pnChanID);

	for (unsigned nChanInd=0;nChanInd<nNumChans;nChanInd++) {
		unsigned nChanID = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
		unsigned nChanDataLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		CString strField;
		strField.Format(_T("Channel index #%u"),nChanInd);
		strVal.Format(_T("ID=%5u DataLength=0x%08X"),nChanID,nChanDataLen);
		PhotoshopParseReportFldStr(nIndent,strField,strVal);
		pLayerInfo->pnChanID[nChanInd] = nChanID;
		pLayerInfo->pnChanLen[nChanInd] = nChanDataLen;
	}
	CString strBlendModeSig = m_pWBuf->BufReadStrn(nPos,4); nPos+=4;
	PhotoshopParseReportFldStr(nIndent,_T("Blend mode signature"),strBlendModeSig);
	unsigned nBlendModeKey = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldEnum(nIndent,_T("Blend mode key"),BIM_T_ENUM_BLEND_MODE_KEY,nBlendModeKey);
	unsigned nOpacity = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Opacity"),nOpacity,_T("(0=transparent ... 255=opaque)"));
	m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);	// unsigned nClipping
	m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);	// unsigned nFlags
	m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);	// unsigned nFiller
	unsigned nExtraDataLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	unsigned long nPosExtra = nPos;
	if (bDecOk)
		bDecOk &= PhotoshopParseLayerMask(nPos,nIndent);
	if (bDecOk)
		bDecOk &= PhotoshopParseLayerBlendingRanges(nPos,nIndent);

	if (bDecOk) {
		unsigned nLayerNameLen = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
		CString strLayerName = m_pWBuf->BufReadStrn(nPos,nLayerNameLen); nPos+=nLayerNameLen;
		// According to spec, length is padded to multiple of 4 bytes,
		unsigned nSkip = 0;
		nSkip = (4 - ((1 + nLayerNameLen) % 4)) %4;
		nPos += nSkip;
	}
	
	// Now use up the "extra bytes" with "Additional Layer Info"
	while ((bDecOk)&&(nPos < nPosExtra + nExtraDataLen)) {
		if (bDecOk)
			bDecOk &= PhotoshopParseAddtlLayerInfo(nPos,nIndent);
	}

	return bDecOk;
}

// Parse the Photoshop IRB Layer Mask
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
bool CDecodePs::PhotoshopParseLayerMask(unsigned long &nPos,unsigned nIndent)
{
	CString		strVal;
	bool		bDecOk = true;

	PhotoshopParseReportNote(nIndent,_T("Layer Mask / Adjustment layer data:"));
	nIndent++;

	unsigned nLayerMaskLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	if (nLayerMaskLen == 0) {
		return bDecOk;
	}

	unsigned	nRectA1,nRectA2,nRectA3,nRectA4;
	unsigned	nRectB1,nRectB2,nRectB3,nRectB4;
	nRectA1 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	nRectA2 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	nRectA3 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	nRectA4 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);

	unsigned	nTmp;
	m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);	// unsigned nDefaultColor
	unsigned nFlags = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);

	//FIXME: Is this correct?
	if (nLayerMaskLen == 20) {
		m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);	// unsigned nPad
	}

	if (nFlags & (1<<4)) {
		unsigned nMaskParams = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
		if (nMaskParams & (1<<0)) {
			// User mask density, 1 byte
			nTmp = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
		}
		if (nMaskParams & (1<<1)) {
			// User mask feather, 8 bytes, double
			nTmp = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
			nTmp = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		}
		if (nMaskParams & (1<<2)) {
			// Vector mask density, 1 byte
			nTmp = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
		}
		if (nMaskParams & (1<<3)) {
			// Vector mask feather, 8 bytes, double
			nTmp = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
			nTmp = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		}

		m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);	// unsigned nPadding
		m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);	// unsigned nRealFlags
		m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);	// unsigned nRealUserMaskBackground
		nRectB1 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		nRectB2 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		nRectB3 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		nRectB4 = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);

	}

	return bDecOk;
}

// Parse the Photoshop IRB Layer blending ranges data
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
bool CDecodePs::PhotoshopParseLayerBlendingRanges(unsigned long &nPos,unsigned nIndent)
{
	CString		strVal;
	bool		bDecOk = true;

	PhotoshopParseReportNote(nIndent,_T("Layer blending ranges data:"));
	nIndent++;

	unsigned nLayerBlendLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	if (nLayerBlendLen == 0) {
		return bDecOk;
	}

	m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);	// unsigned nCompGrayBlendSrc
	m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);	// unsigned nCompGrayBlendDstRng
	unsigned	nNumChans;
	nNumChans = (nLayerBlendLen - 8) /8;

	for (unsigned nChanInd=0;nChanInd<nNumChans;nChanInd++) {
		m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);	// unsigned nSrcRng
		m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);	// unsigned nDstRng
	}
	return bDecOk;
}

// Parse the Photoshop IRB Channel image data
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// - nChan      = Channel being parsed (used for DIB mapping)
// - pDibTemp = DIB for rendering (NULL if disabled)
// OUTPUT:
// - nPos		= File position after reading the block
//
bool CDecodePs::PhotoshopParseChannelImageData(unsigned long &nPos,unsigned nIndent,unsigned nWidth,unsigned nHeight,unsigned nChan,unsigned char* pDibBits)
{
	bool	bDecOk = true;

	unsigned nCompressionMethod = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent+1,_T("Compression method"),nCompressionMethod,_T(""));


	if (nCompressionMethod == 1) {
		// *** RLE Compression ***
		if (nHeight == 0) {
			return true;
		}
		// Read the line lengths into an array
		// -   LOOP [nHeight]
		// -     16-bit: row length
		// -   ENDLOOP
		unsigned*	anRowLen;
		anRowLen = new unsigned [nHeight];
		ASSERT(anRowLen);
		for (unsigned nRow=0;nRow<nHeight;nRow++) {
			anRowLen[nRow] = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
		}
	
		// Read the compressed data
		for (unsigned nRow=0;(bDecOk)&&(nRow<nHeight);nRow++) {
			unsigned nRowLen = anRowLen[nRow];
			bDecOk = PhotoshopDecodeRowRle(nPos,nWidth,nHeight,nRow,nRowLen,nChan,pDibBits);
		} //nRow

		// Deallocate
		if (anRowLen) {
			delete [] anRowLen;
			anRowLen = NULL;
		}

	} else if (nCompressionMethod == 0) {
		// *** RAW (no compression)
		if (nHeight == 0) {
			return true;
		}
		for (unsigned nRow=0;(bDecOk)&&(nRow<nHeight);nRow++) {
			bDecOk = PhotoshopDecodeRowUncomp(nPos,nWidth,nHeight,nRow,nChan,pDibBits);
		}
		
	} else {
		m_pLog->AddLineWarn(_T("Unsupported compression method. Stopping."));
		bDecOk = false;
		return bDecOk;
	}

	return bDecOk;
}


bool CDecodePs::PhotoshopDecodeRowUncomp(unsigned long &nPos,unsigned nWidth,unsigned nHeight,unsigned nRow,unsigned nChanID,unsigned char* pDibBits)
{
	bool			bDecOk = true;
	unsigned char	nVal;
	unsigned		nRowActual;
	unsigned		nPixByte;

	for (unsigned nCol=0;(bDecOk)&&(nCol<nWidth);nCol++) {
		nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);

#ifdef PS_IMG_DEC_EN
		if (pDibBits) {
			nRowActual = nHeight-nRow-1;	// Need to flip vertical for DIB
			nPixByte = (nRowActual*nWidth + nCol)*sizeof(RGBQUAD);

			// Assign the RGB pixel map
			pDibBits[nPixByte+3] = 0;
			if (nChanID == 0) {
				pDibBits[nPixByte+2] = nVal;
			} else if (nChanID == 1) {
				pDibBits[nPixByte+1] = nVal;
			} else if (nChanID == 2) {
				pDibBits[nPixByte+0] = nVal;
			} else {
			}
		} // pDibBits
#endif

	} // nCol


	return bDecOk;
}

bool CDecodePs::PhotoshopDecodeRowRle(unsigned long &nPos,unsigned nWidth,unsigned nHeight,unsigned nRow,unsigned nRowLen,unsigned nChanID,unsigned char* pDibBits)
{
	bool			bDecOk = true;

	unsigned char	nRleRun;
	signed char		nRleRunS;
	unsigned		nRleRunCnt;
	unsigned char	nRleVal;
	unsigned		nRowOffsetComp;		// Row offset (compressed size)
	unsigned		nRowOffsetDecomp;	// Row offset (decompressed size)
	unsigned		nRowOffsetDecompLast;
	unsigned		nRowActual;
	unsigned		nPixByte;

	// Decompress the row data
	nRowOffsetComp = 0;
	nRowOffsetDecomp = 0;
	nRowOffsetDecompLast = 0;
	while ((bDecOk)&&(nRowOffsetComp<nRowLen)) {

		// Save the position in the row before decompressing
		// the current RLE encoded entry
		nRowOffsetDecompLast = nRowOffsetDecomp;

		nRleRun = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
		nRleRunS = (signed char)(nRleRun);
		nRowOffsetComp++;

		if (nRleRunS<0) {
			// Replicate the next byte
			nRleRunCnt = 1-nRleRunS;
			nRleVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
			nRowOffsetComp++;
			nRowOffsetDecomp += nRleRunCnt;

#ifdef PS_IMG_DEC_EN
			if (pDibBits) {
				nRowActual = nHeight-nRow-1;	// Need to flip vertical for DIB
				for (unsigned nRunInd=0;nRunInd<nRleRunCnt;nRunInd++) {
					nPixByte = (nRowActual*nWidth + nRowOffsetDecompLast+nRunInd)*sizeof(RGBQUAD);
	
					// Assign the RGB pixel map
					pDibBits[nPixByte+3] = 0;
					if (nChanID == 0) {
					pDibBits[nPixByte+2] = nRleVal;
					} else if (nChanID == 1) {
						pDibBits[nPixByte+1] = nRleVal;
					} else if (nChanID == 2) {
						pDibBits[nPixByte+0] = nRleVal;
					} else {
					}
				} // nRunInd
			} // pDibBits
#endif

		} else {

			// Copy the next bytes as-is
			nRleRunCnt = 1+nRleRunS;
			for (unsigned nRunInd=0;nRunInd<nRleRunCnt;nRunInd++) {
				nRleVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
				nRowOffsetComp++;
				nRowOffsetDecomp++;

#ifdef PS_IMG_DEC_EN
				if (pDibBits) {
					nRowActual = nHeight-nRow-1;	// Need to flip vertical for DIB
					nPixByte = (nRowActual*nWidth + nRowOffsetDecompLast+nRunInd)*sizeof(RGBQUAD);

					// Assign the RGB pixel map
					pDibBits[nPixByte+3] = 0;
					if (nChanID == 0) {
						pDibBits[nPixByte+2] = nRleVal;
					} else if (nChanID == 1) {
						pDibBits[nPixByte+1] = nRleVal;
					} else if (nChanID == 2) {
						pDibBits[nPixByte+0] = nRleVal;
					} else {
					}
				} // pDibBits
#endif

			} // nRunInd
		} // nRleRunS

	} // nRowOffsetComp
	
	// Now that we've finished the row, compare the decompressed size
	// to the expected width
	if (nRowOffsetDecomp != nWidth) {
		bDecOk = false;
		ASSERT(false);
	}

	return bDecOk;
}

// Parse the Photoshop IRB Layer and Mask Info Section (from File Header)
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
// NOTE:
// - Image decoding (into DIB) is enabled if pDibBits is not NULL
//
bool CDecodePs::PhotoshopParseImageData(unsigned long &nPos,unsigned nIndent,tsImageInfo* psImageInfo,unsigned char* pDibBits)
{
	ASSERT(psImageInfo);

	CString		strVal;

	//PhotoshopParseReportNote(nIndent,_T("Image data section:"));
	PhotoshopParseReportFldOffset(nIndent,_T("Image data section:"),nPos);


	bool	bDecOk = true;

	//----
	unsigned	nWidth = psImageInfo->nImageWidth;
	unsigned	nHeight = psImageInfo->nImageHeight;
	unsigned	nNumChans = psImageInfo->nNumChans;
	//----

	unsigned nCompressionMethod = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent+1,_T("Compression method"),nCompressionMethod,_T(""));

	// -----------------------------------------------------

	if (nCompressionMethod == 1) {
		// *** RLE Compression ***
		if (nHeight == 0) {
			return true;
		}

		// Read the line lengths into an array
		// - LOOP [nNumChans]
		// -   LOOP [nHeight]
		// -     16-bit: row length
		// -   ENDLOOP
		// - ENDLOOP
		unsigned*	anRowLen;
		anRowLen = new unsigned [nNumChans*nHeight];
		ASSERT(anRowLen);
		for (unsigned nRow=0;nRow<(nNumChans*nHeight);nRow++) {
			anRowLen[nRow] = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
		}
	
		// Read the compressed data
		for (unsigned nChan=0;nChan<nNumChans;nChan++) {
			for (unsigned nRow=0;(bDecOk)&&(nRow<nHeight);nRow++) {
				unsigned nRowLen = anRowLen[(nChan*nHeight)+nRow];
				bDecOk = PhotoshopDecodeRowRle(nPos,nWidth,nHeight,nRow,nRowLen,nChan,pDibBits);
			} // nRow
		} // nChan

		// Deallocate
		if (anRowLen) {
			delete [] anRowLen;
			anRowLen = NULL;
		}

	} else if (nCompressionMethod == 0) {
		// *** RAW (no compression)

		// NOTE: BUG BUG BUG
		// It seems that most examples I see with RAW layers have nHeight=nWidth=0
		// or else the nChan=0xFFFF. Why aren't width & height defined?

		if (nHeight*nNumChans == 0) {
			return true;
		}
		for (unsigned nChan=0;nChan<nNumChans;nChan++) {
			for (unsigned nRow=0;(bDecOk)&&(nRow<nHeight);nRow++) {
				bDecOk = PhotoshopDecodeRowUncomp(nPos,nWidth,nHeight,nRow,nChan,pDibBits);
			} //nRow
		} //nChan
		
	} else {
		m_pLog->AddLineWarn(_T("Unsupported compression method. Stopping."));
		bDecOk = false;
		return bDecOk;
	}


	return bDecOk;

}

// Parse the Photoshop IRB Global layer mask info
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
bool CDecodePs::PhotoshopParseGlobalLayerMaskInfo(unsigned long &nPos,unsigned nIndent)
{
	CString		strVal;
	bool		bDecOk = true;

	PhotoshopParseReportNote(nIndent,_T("Global layer mask info:"));
	nIndent++;

	unsigned nInfoLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	if (nInfoLen == 0) {
		return bDecOk;
	}
	unsigned long nPosStart = nPos;
	m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);	// unsigned nOverlayColorSpace
	m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);	// unsigned nColComp1
	m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);	// unsigned nColComp2
	m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);	// unsigned nColComp3
	m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);	// unsigned nColComp4
	m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);	// unsigned nOpacity
	m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);	// unsigned nKind

	// Variable Filler: zeros
	//nPos += nInfoLen - 17;
	nPos = nPosStart + nInfoLen;

	return bDecOk;
}

// Parse the Photoshop IRB Additional layer info
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
bool CDecodePs::PhotoshopParseAddtlLayerInfo(unsigned long &nPos,unsigned nIndent)
{
	bool			bDecOk = true;
	unsigned long	nPosStart;

	PhotoshopParseReportNote(nIndent,_T("Additional layer info:"));
	nIndent++;

	CString strSig = m_pWBuf->BufReadStrn(nPos,4); nPos+=4;

	if (strSig != _T("8BIM")) {
		// Signature did not match!
		CString strError;
		strError.Format(_T("ERROR: Addtl Layer Info signature unknown [%s] @ 0x%08X"),(LPCTSTR)strSig,nPos-4);
		PhotoshopParseReportNote(nIndent,strError);

#ifdef DEBUG_LOG
		CString	strDebug;
		strDebug.Format(_T("## File=[%-100s] Block=[%-10s] Error=[%s]\n"),(LPCTSTR)m_pAppConfig->strCurFname,
			_T("PsDecode"),(LPCTSTR)strError);
		OutputDebugString(strDebug);
#else
		ASSERT(false);
#endif
		bDecOk = false;
		return false;
	}

	CString strKey = m_pWBuf->BufReadStrn(nPos,4); nPos+=4;
	PhotoshopParseReportFldStr(nIndent,_T("Key"),strKey);
	bool bLen8B = false;
	if (strKey == _T("Lr16")) {
		// FIXME: Might need some additional processing here?
		ASSERT(false);
	}
	/*
	//CAL! FIXME: Empirically, I didn't see 8B length being used by "FMsk" in psd-flame-icon.psd @ 0x7A458
	if (strKey == _T("LMsk")) { bLen8B = true; }
	if (strKey == _T("Lr16")) { bLen8B = true; }
	if (strKey == _T("Lr32")) { bLen8B = true; }
	if (strKey == _T("Layr")) { bLen8B = true; }
	if (strKey == _T("Mt16")) { bLen8B = true; }
	if (strKey == _T("Mt32")) { bLen8B = true; }
	if (strKey == _T("Mtrn")) { bLen8B = true; }
	if (strKey == _T("Alph")) { bLen8B = true; }
	if (strKey == _T("FMsk")) { bLen8B = true; }
	if (strKey == _T("lnk2")) { bLen8B = true; }
	if (strKey == _T("FEid")) { bLen8B = true; }
	if (strKey == _T("FXid")) { bLen8B = true; }
	if (strKey == _T("PxSD")) { bLen8B = true; }
	*/

	unsigned nLen;
	if (!bLen8B) {
		nLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	} else {
		// DUMMY for now
		//FIXME: Untested
		nLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		ASSERT(nLen == 0);
		nLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	}
	PhotoshopParseReportFldNum(nIndent,_T("Length"),nLen,_T(""));
	if (nLen > 0) {
		PhotoshopParseReportFldHex(nIndent,strKey,nPos,nLen);
	}

	unsigned	nVal;
	CString		sStr;
	unsigned	nPosOffset = 0;
	if (strKey == _T("luni")) {
		sStr = PhotoshopParseGetBimLStrUni(nPos,nPosOffset);
		PhotoshopParseReportFldStr(nIndent,_T("Layer Name (Unicode)"),sStr);
	} else if (strKey == _T("lnsr")) {
		nVal = m_pWBuf->BufX(nPos,4,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent,_T("Layer Name Source ID"),nVal,_T(""));
	} else if (strKey == _T("lyid")) {
		nVal = m_pWBuf->BufX(nPos,4,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent,_T("Layer ID"),nVal,_T(""));
	} else if (strKey == _T("clbl")) {
		nVal = m_pWBuf->BufX(nPos,4,PS_BSWAP);
		PhotoshopParseReportFldBool(nIndent,_T("Blend clipped elements"),nVal);
	} else if (strKey == _T("infx")) {
		nVal = m_pWBuf->BufX(nPos,4,PS_BSWAP);
		PhotoshopParseReportFldBool(nIndent,_T("Blend interior elements"),nVal);
	} else if (strKey == _T("knko")) {
		nVal = m_pWBuf->BufX(nPos,4,PS_BSWAP);
		PhotoshopParseReportFldBool(nIndent,_T("Knockout"),nVal);
	}

	nPosStart = nPos;

	// Skip data
	nPos = nPosStart + nLen;

	// FIXME: It appears that we need to pad out to ensure that
	// length is multiple of 4 (not file position).
	unsigned nMod;
	nMod = (nLen%4);
	if (nMod != 0) {
		nPos += (4-nMod);
	}


	return bDecOk;
}



// -------------


// Parse the Photoshop IRB Image Resources Section
// - Iteratively works through the individual Image Resource Blocks (IRB)
//
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
bool CDecodePs::PhotoshopParseImageResourcesSection(unsigned long &nPos,unsigned nIndent)
{
	CString		strVal;
	bool		bDecOk = true;

	PhotoshopParseReportNote(nIndent,_T("Image Resources Section:"));
	nIndent++;

	unsigned long	nPosSectionStart = 0;
	unsigned long	nPosSectionEnd = 0;

	unsigned nImgResLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Length"),nImgResLen,_T(""));

	nPosSectionStart = nPos;
	nPosSectionEnd = nPos + nImgResLen;

	while (nPos < nPosSectionEnd) {
		bDecOk &= PhotoshopParseImageResourceBlock(nPos,nIndent);
		if (!bDecOk) { return false; }
	}
	return bDecOk;
}

// Parse the Photoshop IRB (Image Resource Block)
// - Decode the 8BIM ID and invoke the appropriate IRB parsing handler
//
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
bool CDecodePs::PhotoshopParseImageResourceBlock(unsigned long &nPos,unsigned nIndent)
{
	CString		strVal;
	//bool		bDecOk = true;

	//PhotoshopParseReportNote(nIndent,_T("Image Resource Block:"));
	//nIndent++;

	//
	// - Signature	(4B)						"8BIM"
	// - NameLen	(2B)						Generally 0
	// - Name		(NameLen Bytes, min 1 Byte)	Usually empty (NULL)
	// - BimLen		(4B)						Length of 8BIM data
	// - Data		(1..BimLen)					Field data
	//

	CString strSig = m_pWBuf->BufReadStrn(nPos,4); nPos+=4;

	if (strSig != _T("8BIM")) {
		// Signature did not match!
		CString strError;
		strError.Format(_T("ERROR: IRB signature unknown [%s]"),(LPCTSTR)strSig);
		PhotoshopParseReportNote(nIndent,strError);

#ifdef DEBUG_LOG
		CString	strDebug;
		strDebug.Format(_T("## File=[%-100s] Block=[%-10s] Error=[%s]\n"),(LPCTSTR)m_pAppConfig->strCurFname,
			_T("PsDecode"),(LPCTSTR)strError);
		OutputDebugString(strDebug);
#else
		ASSERT(false);
#endif

		return false;;
	}

	unsigned nBimId = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);

	unsigned nResNameLen = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	CString strResName = m_pWBuf->BufReadStrn(nPos,nResNameLen); nPos+=nResNameLen;

	// If size wasn't even, pad here
	if ((1+nResNameLen)%2!=0) {
		nPos+=1;
	}
	unsigned nBimLen = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);

	CString		strTmp;
	CString		strBimName;
	CString		strByte;

	// Lookup 8BIM defined name
	CString		strBimDefName = _T("");
	bool		bBimKnown = false;
	unsigned	nFldInd = 0;
	bBimKnown = FindBimRecord(nBimId,nFldInd);

	if (bBimKnown) {
		strBimDefName = asBimRecords[nFldInd].strRecordName;
	}
	
	strTmp.Format(_T("8BIM: [0x%04X] Name=\"%s\" Len=[0x%04X] DefinedName=\"%s\""),
		nBimId,(LPCTSTR)strBimName,nBimLen,(LPCTSTR)strBimDefName);
	PhotoshopParseReportNote(nIndent,strTmp);
	
	nIndent++;

	if (nBimLen == 0) {
		// If length is zero, then skip this block
		PhotoshopParseReportNote(nIndent,_T("Length is zero. Skipping."));

	} else if (bBimKnown) {

		// Save the file pointer
		unsigned nPosSaved;
		nPosSaved = nPos;

		// Calculate the end of the record
		// - This is used for parsing records that have a conditional parsing
		//   of additional fields "if length permits".
		unsigned nPosEnd;
		nPosEnd = nPos + nBimLen - 1;

		switch (asBimRecords[nFldInd].eBimType) {

		case BIM_T_STR:
			strVal = DecodeIptcValue(IPTC_T_STR,nBimLen,nPos);
			PhotoshopParseReportFldStr(nIndent,strBimDefName,strVal);
			break;
		case BIM_T_HEX:
			PhotoshopParseReportFldHex(nIndent,strBimDefName,nPos,nBimLen);
			nPos += nBimLen;
			break;


		case BIM_T_PS_THUMB_RES:
			PhotoshopParseThumbnailResource(nPos,nIndent);
			// Since this block has image data that we are not going to parse,
			// force the end position to match the length
			nPos = nPosSaved + nBimLen;
			break;
		case BIM_T_PS_SLICES:
			PhotoshopParseSliceHeader(nPos,nIndent,nPosEnd);
			break;
		case BIM_T_PS_DESCRIPTOR:
			PhotoshopParseDescriptor(nPos,nIndent);
			break;
		case BIM_T_PS_VER_INFO:
			PhotoshopParseVersionInfo(nPos,nIndent);
			break;
		case BIM_T_PS_PRINT_SCALE:
			PhotoshopParsePrintScale(nPos,nIndent);
			break;
		case BIM_T_PS_PIXEL_ASPECT_RATIO:
			PhotoshopParsePixelAspectRatio(nPos,nIndent);
			break;
		case BIM_T_PS_DOC_SPECIFIC_SEED:
			PhotoshopParseDocSpecificSeed(nPos,nIndent);
			break;
		case BIM_T_PS_RESOLUTION_INFO:
			PhotoshopParseResolutionInfo(nPos,nIndent);
			break;
		case BIM_T_PS_GRID_GUIDES:
			PhotoshopParseGridGuides(nPos,nIndent);
			break;
		case BIM_T_PS_GLOBAL_ANGLE:
			PhotoshopParseGlobalAngle(nPos,nIndent);
			break;
		case BIM_T_PS_GLOBAL_ALTITUDE:
			PhotoshopParseGlobalAltitude(nPos,nIndent);
			break;
		case BIM_T_PS_PRINT_FLAGS:
			PhotoshopParsePrintFlags(nPos,nIndent);
			break;
		case BIM_T_PS_PRINT_FLAGS_INFO:
			PhotoshopParsePrintFlagsInfo(nPos,nIndent);
			break;
		case BIM_T_PS_COPYRIGHT_FLAG:
			PhotoshopParseCopyrightFlag(nPos,nIndent);
			break;
		case BIM_T_PS_LAYER_STATE_INFO:
			PhotoshopParseLayerStateInfo(nPos,nIndent);
			break;
		case BIM_T_PS_LAYER_GROUP_INFO:
			PhotoshopParseLayerGroupInfo(nPos,nIndent,nBimLen);
			break;
		case BIM_T_PS_LAYER_GROUP_ENABLED:
			PhotoshopParseLayerGroupEnabled(nPos,nIndent,nBimLen);
			break;
		case BIM_T_PS_LAYER_SELECT_ID:
			PhotoshopParseLayerSelectId(nPos,nIndent);
			break;

		case BIM_T_PS_STR_UNI:
			PhotoshopParseStringUni(nPos,nIndent);
			break;
		case BIM_T_PS_STR_ASC:
			strVal += m_pWBuf->BufReadStrn(nPos,nBimLen);
			nPos += nBimLen;
			PhotoshopParseReportFldStr(nIndent,strBimDefName,strVal);
			break;
		case BIM_T_PS_STR_ASC_LONG:
			strVal = _T("\n");
			strVal += m_pWBuf->BufReadStrn(nPos,nBimLen);
			nPos += nBimLen;
			PhotoshopParseReportFldStr(nIndent,strBimDefName,strVal);
			break;

		case BIM_T_JPEG_QUAL: // (0x0406)
			// JPEG Quality
			PhotoshopParseJpegQuality(nPos,nIndent,nPosEnd);
			break;

		case BIM_T_IPTC_NAA: // (0x0404)
			// IPTC-NAA
			DecodeIptc(nPos,nBimLen,nIndent);
			break;

		default:
			return false;
			break;
		} // switch

		// Check to see if we ended up with a length mismatch after decoding
		if (nPos > nPosEnd+1) {
			// Length mismatch detected: we read too much (versus length)
			strTmp.Format(_T("ERROR: Parsing exceeded expected length. Stopping decode. BIM=[%s], CurPos=[0x%08X], ExpPosEnd=[0x%08X], ExpLen=[%u]"),
				(LPCTSTR)strBimDefName,nPos,nPosEnd+1,nBimLen);
			m_pLog->AddLineErr(strTmp);
#ifdef DEBUG_LOG
			CString	strDebug;
			strDebug.Format(_T("## File=[%-100s] Block=[%-10s] Error=[%s]\n"),(LPCTSTR)m_pAppConfig->strCurFname,
				_T("PsDecode"),(LPCTSTR)strTmp);
			OutputDebugString(strDebug);
#else
			ASSERT(false);
#endif

			// TODO: Add interactive error message here

			// Now we should roll-back the file position to the position indicated
			// by the length. This will help us stay on track for next decoding.
			nPos = nPosEnd+1;

			return false;
		} else if (nPos != nPosEnd+1) {
			// Length mismatch detected: we read too little (versus length)
			// This is generally an indication that either I haven't accurately captured the
			// specific block parsing format or else the specification is loose.
			strTmp.Format(_T("WARNING: Parsing offset length mismatch. Current pos=[0x%08X], expected end pos=[0x%08X], expect length=[%u]"),
				nPos,nPosEnd+1,nBimLen);
			m_pLog->AddLineWarn(strTmp);
#ifdef DEBUG_LOG
			CString	strDebug;
			strDebug.Format(_T("## File=[%-100s] Block=[%-10s] Error=[%s]\n"),(LPCTSTR)m_pAppConfig->strCurFname,
				_T("PsDecode"),(LPCTSTR)strTmp);
			OutputDebugString(strDebug);
#endif
			return false;
		}

		// Reset the file marker
		nPos = nPosSaved;
	} // bBimKnown

	// Skip rest of 8BIM
	nPos += nBimLen;

	// FIXME: correct to make for even parity?
	if ((nBimLen % 2) != 0) nPos++;
	
	return true;
}


// Parse the Photoshop IRB Slice Header
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// - nPosEnd	= File position at end of block (last byte address)
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseSliceHeader(unsigned long &nPos,unsigned nIndent,unsigned long nPosEnd)
{
	unsigned	nVal;
	CString		strVal;
	unsigned	nPosOffset;

	PhotoshopParseReportNote(nIndent,_T("Slice Header:"));
	nIndent++;

	unsigned nSliceVer = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Version"),nSliceVer,_T(""));

	if (nSliceVer == 6) {
		nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent,_T("Bound Rect (top)"),nVal,_T(""));
		nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent,_T("Bound Rect (left)"),nVal,_T(""));
		nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent,_T("Bound Rect (bottom)"),nVal,_T(""));
		nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent,_T("Bound Rect (right)"),nVal,_T(""));
		strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset);
		nPos+=nPosOffset;
		PhotoshopParseReportFldStr(nIndent,_T("Name of group of slices"),strVal);
		unsigned nNumSlices = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent,_T("Number of slices"),nNumSlices,_T(""));

		// Slice resource block
		if (nNumSlices>0)
			PhotoshopParseReportNote(nIndent,_T("-----"));
		for (unsigned nSliceInd=0;nSliceInd<nNumSlices;nSliceInd++) {
			CString strSliceInd;
			strSliceInd.Format(_T("Slice #%u:"),nSliceInd);
			PhotoshopParseReportNote(nIndent,strSliceInd);
			PhotoshopParseSliceResource(nPos,nIndent+1,nPosEnd);
		}
		if (nNumSlices>0)
			PhotoshopParseReportNote(nIndent,_T("-----"));

	} else if ((nSliceVer == 7) || (nSliceVer == 8)) {
		nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent,_T("Descriptor version"),nVal,_T(""));
		PhotoshopParseDescriptor(nPos,nIndent);
	}


}

// Parse the Photoshop IRB Slice Resource
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// - nPosEnd	= File position at end of block (last byte address)
// OUTPUT:
// - nPos		= File position after reading the block
// NOTE:
// - The nPosEnd is supplied as this resource block depends on some
//   conditional field parsing that is "as length allows"
//
void CDecodePs::PhotoshopParseSliceResource(unsigned long &nPos,unsigned nIndent,unsigned long nPosEnd)
{
	unsigned	nVal;
	CString		strVal;
	unsigned	nPosOffset;

	PhotoshopParseReportNote(nIndent,_T("Slice Resource:"));
	nIndent++;
	
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("ID"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Group ID"),nVal,_T(""));
	unsigned nOrigin = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Origin"),nOrigin,_T(""));
	if (nOrigin==1) {
		nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent,_T("Associated Layer ID"),nVal,_T(""));
	}
	strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset); nPos+=nPosOffset;
	PhotoshopParseReportFldStr(nIndent,_T("Name"),strVal);
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Type"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Position (top)"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Position (left)"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Position (bottom)"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Position (right)"),nVal,_T(""));
	strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset); nPos+=nPosOffset;
	PhotoshopParseReportFldStr(nIndent,_T("URL"),strVal);
	strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset); nPos+=nPosOffset;
	PhotoshopParseReportFldStr(nIndent,_T("Target"),strVal);
	strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset); nPos+=nPosOffset;
	PhotoshopParseReportFldStr(nIndent,_T("Message"),strVal);
	strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset); nPos+=nPosOffset;
	PhotoshopParseReportFldStr(nIndent,_T("Alt Tag"),strVal);
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Cell text is HTML"),nVal);
	strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset); nPos+=nPosOffset;
	PhotoshopParseReportFldStr(nIndent,_T("Cell text"),strVal);
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Horizontal alignment"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Vertical alignment"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Alpha color"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Red"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Green"),nVal,_T(""));
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Blue"),nVal,_T(""));

	// Per standard, we are only supposed to parse additional fields
	// "as length allows". This is not a particularly good format
	// definition. At this point we'll perform a check to see if
	// we have exhausted the length of the block. If not, continue
	// to parse the next fields.
	// TODO: Do we need to be any more precise than this? (ie. check
	// after reading the descriptor version?)
	if (nPos <= nPosEnd) {

		nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
		PhotoshopParseReportFldNum(nIndent,_T("Descriptor version"),nVal,_T(""));

		PhotoshopParseDescriptor(nPos,nIndent);
	} else {
		// We ran out of space in the block so end now
	}
}


// Parse the Photoshop IRB JPEG Quality
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// - nPosEnd	= File position at end of block (last byte address)
// OUTPUT:
// - nPos		= File position after reading the block
// NOTE:
// - This IRB is private, so reverse-engineered and may not be per spec
//
void CDecodePs::PhotoshopParseJpegQuality(unsigned long &nPos,unsigned nIndent,unsigned long nPosEnd)
{
	nPosEnd;	// Unreferenced param

	unsigned	nVal;
	CString		strVal;
	unsigned	nSaveFormat;

	// Save As Quality
	// Index 0: Quality level
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	switch(nVal) {
		case 0xFFFD: m_nQualitySaveAs = 1; break;
		case 0xFFFE: m_nQualitySaveAs = 2; break;
		case 0xFFFF: m_nQualitySaveAs = 3; break;
		case 0x0000: m_nQualitySaveAs = 4; break;
		case 0x0001: m_nQualitySaveAs = 5; break;
		case 0x0002: m_nQualitySaveAs = 6; break;
		case 0x0003: m_nQualitySaveAs = 7; break;
		case 0x0004: m_nQualitySaveAs = 8; break;
		case 0x0005: m_nQualitySaveAs = 9; break;
		case 0x0006: m_nQualitySaveAs = 10; break;
		case 0x0007: m_nQualitySaveAs = 11; break;
		case 0x0008: m_nQualitySaveAs = 12; break;
		default: m_nQualitySaveAs = 0; break;
	}
	if (m_nQualitySaveAs != 0) {
		PhotoshopParseReportFldNum(nIndent,_T("Photoshop Save As Quality"),m_nQualitySaveAs,_T(""));
	}

	// TODO: I have observed a file that had this block with length=2, which would end here
	// - Perhaps we should check against length in case some older files had a shorter format
	//   to this IRB?

	// Index 1: Format
	nSaveFormat = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	switch(nSaveFormat) {
		case 0x0000: strVal = _T("Standard"); break;
		case 0x0001: strVal = _T("Optimized"); break;
		case 0x0101: strVal = _T("Progressive"); break;
		default: strVal = _T("???"); break;
	}
	PhotoshopParseReportFldStr(nIndent,_T("Photoshop Save Format"),strVal);

	// Index 2: Progressive Scans
	// - Only meaningful if Progressive mode
	nVal = m_pWBuf->BufRdAdv2(nPos,PS_BSWAP);
	switch(nVal) {
		case 0x0001: strVal = _T("3 Scans"); break;
		case 0x0002: strVal = _T("4 Scans"); break;
		case 0x0003: strVal = _T("5 Scans"); break;
		default: strVal = _T("???"); break;
	}
	PhotoshopParseReportFldStr(nIndent,_T("Photoshop Save Progressive Scans"),strVal);

	// Not sure what this byte is for
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("???"),nVal,_T(""));

}

// Decode the OSType field and invoke the appropriate IRB parsing handler
//
// INPUT:
// - nPos		= File position at start of block
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the block
//
void CDecodePs::PhotoshopParseHandleOsType(CString strOsType,unsigned long &nPos,unsigned nIndent)
{
	if        (strOsType==_T("obj ")) {
		//PhotoshopParseReference(nPos,nIndent);
	} else if (strOsType==_T("Objc")) {
		PhotoshopParseDescriptor(nPos,nIndent);
	} else if (strOsType==_T("VlLs")) {
		PhotoshopParseList(nPos,nIndent);
	} else if (strOsType==_T("doub")) {
		//PhotoshopParseDouble(nPos,nIndent);
	} else if (strOsType==_T("UntF")) {
		//PhotoshopParseUnitFloat(nPos,nIndent);
	} else if (strOsType==_T("TEXT")) {
		PhotoshopParseStringUni(nPos,nIndent);
	} else if (strOsType==_T("enum")) {
		PhotoshopParseEnum(nPos,nIndent);
	} else if (strOsType==_T("long")) {
		PhotoshopParseInteger(nPos,nIndent);
	} else if (strOsType==_T("bool")) {
		PhotoshopParseBool(nPos,nIndent);
	} else if (strOsType==_T("GlbO")) {
		PhotoshopParseDescriptor(nPos,nIndent);
	} else if (strOsType==_T("type")) {
		//PhotoshopParseClass(nPos,nIndent);
	} else if (strOsType==_T("GlbC")) {
		//PhotoshopParseClass(nPos,nIndent);
	} else if (strOsType==_T("alis")) {
		//PhotoshopParseAlias(nPos,nIndent);
	} else if (strOsType==_T("tdta")) {
		//PhotoshopParseRawData(nPos,nIndent);
	} else {

#ifdef DEBUG_LOG
		CString	strError;
		CString	strDebug;
		strError.Format(_T("ERROR: Unsupported OSType [%s]"),(LPCTSTR)strOsType);
		strDebug.Format(_T("## File=[%-100s] Block=[%-10s] Error=[%s]\n"),(LPCTSTR)m_pAppConfig->strCurFname,
			_T("PsDecode"),(LPCTSTR)strError);
		OutputDebugString(strDebug);
#else
		ASSERT(false);
#endif

	}
}

// Parse the Photoshop IRB OSType Descriptor
// INPUT:
// - nPos		= File position at start of entry
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the entry
//
void CDecodePs::PhotoshopParseDescriptor(unsigned long &nPos,unsigned nIndent)
{
	CString		strVal;
	unsigned	nPosOffset;

	PhotoshopParseReportNote(nIndent,_T("Descriptor:"));
	nIndent++;

	strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset); nPos+=nPosOffset;
	PhotoshopParseReportFldStr(nIndent,_T("Name from classID"),strVal);

	strVal = PhotoshopParseGetLStrAsc(nPos);
	PhotoshopParseReportFldStr(nIndent,_T("classID"),strVal);

	unsigned nDescNumItems = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Num items in descriptor"),nDescNumItems,_T(""));

	if (nDescNumItems>0)
		PhotoshopParseReportNote(nIndent,_T("-----"));
	for (unsigned nDescInd=0;nDescInd<nDescNumItems;nDescInd++) {
		CString strDescInd;
		strDescInd.Format(_T("Descriptor item #%u:"),nDescInd);
		PhotoshopParseReportNote(nIndent,strDescInd);

		strVal = PhotoshopParseGetLStrAsc(nPos);
		PhotoshopParseReportFldStr(nIndent+1,_T("Key"),strVal);

		CString strOsType;
		strOsType.Format(_T("%c%c%c%c"),Buf(nPos+0),Buf(nPos+1),Buf(nPos+2),Buf(nPos+3));
		nPos+=4;
		PhotoshopParseReportFldStr(nIndent+1,_T("OSType key"),strOsType);

		PhotoshopParseHandleOsType(strOsType,nPos,nIndent+1);
	}
	if (nDescNumItems>0)
		PhotoshopParseReportNote(nIndent,_T("-----"));
}

// Parse the Photoshop IRB OSType List
// INPUT:
// - nPos		= File position at start of entry
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the entry
//
void CDecodePs::PhotoshopParseList(unsigned long &nPos,unsigned nIndent)
{
	CString		strVal;
	CString		strLine;

	unsigned nNumItems = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Num items in list"),nNumItems,_T(""));

	if (nNumItems>0)
		PhotoshopParseReportNote(nIndent,_T("-----"));
	for (unsigned nItemInd=0;nItemInd<nNumItems;nItemInd++) {
		CString strItemInd;
		strItemInd.Format(_T("Item #%u:"),nItemInd);
		PhotoshopParseReportNote(nIndent,strItemInd);

		CString strOsType;
		strOsType.Format(_T("%c%c%c%c"),Buf(nPos+0),Buf(nPos+1),Buf(nPos+2),Buf(nPos+3));
		nPos+=4;
		PhotoshopParseReportFldStr(nIndent+1,_T("OSType key"),strVal);

		PhotoshopParseHandleOsType(strOsType,nPos,nIndent+1);
	}
	if (nNumItems>0)
		PhotoshopParseReportNote(nIndent,_T("-----"));
}

// Parse the Photoshop IRB OSType Integer
// INPUT:
// - nPos		= File position at start of entry
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the entry
//
void CDecodePs::PhotoshopParseInteger(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;
	CString		strLine;
	nVal = m_pWBuf->BufRdAdv4(nPos,PS_BSWAP);
	PhotoshopParseReportFldNum(nIndent,_T("Value"),nVal,_T(""));
}

// Parse the Photoshop IRB OSType Boolean
// INPUT:
// - nPos		= File position at start of entry
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the entry
//
void CDecodePs::PhotoshopParseBool(unsigned long &nPos,unsigned nIndent)
{
	unsigned	nVal;
	CString		strVal;
	CString		strLine;
	nVal = m_pWBuf->BufRdAdv1(nPos,PS_BSWAP);
	PhotoshopParseReportFldBool(nIndent,_T("Value"),nVal);
}

// Parse the Photoshop IRB OSType Enumerated type
// INPUT:
// - nPos		= File position at start of entry
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the entry
//
void CDecodePs::PhotoshopParseEnum(unsigned long &nPos,unsigned nIndent)
{
	CString		strVal;
	CString		strLine;

	strVal = PhotoshopParseGetLStrAsc(nPos);
	PhotoshopParseReportFldStr(nIndent,_T("Type"),strVal);

	strVal = PhotoshopParseGetLStrAsc(nPos);
	PhotoshopParseReportFldStr(nIndent,_T("Enum"),strVal);

}

// Parse the Photoshop IRB Unicode String
// INPUT:
// - nPos		= File position at start of entry
// - nIndent	= Indent level for formatted output
// OUTPUT:
// - nPos		= File position after reading the entry
// NOTE:
// - The string is in Photoshop Unicode format (length first)
//
void CDecodePs::PhotoshopParseStringUni(unsigned long &nPos,unsigned nIndent)
{
	CString		strVal;
	unsigned	nPosOffset;

	strVal = PhotoshopParseGetBimLStrUni(nPos,nPosOffset); nPos+=nPosOffset;
	PhotoshopParseReportFldStr(nIndent,_T("String"),strVal);
}


// ===============================================================================
// CONSTANTS
// ===============================================================================


// Define all of the currently-known IPTC fields, used when we
// parse the APP13 marker
//
// - Reference: IPTC-NAA Information Interchange Model Version 4
// - See header for struct encoding
struct tsIptcField asIptcFields[] =
{
	{ 1,  0,IPTC_T_NUM2,  _T("Model Version")},
	{ 1,  5,IPTC_T_STR,   _T("Destination")},
	{ 1, 20,IPTC_T_NUM2,  _T("File Format")},
	{ 1, 22,IPTC_T_NUM2,  _T("File Format Version")},
	{ 1, 30,IPTC_T_STR,   _T("Service Identifier")},
	{ 1, 40,IPTC_T_STR,   _T("Envelope Number")},
	{ 1, 50,IPTC_T_STR,   _T("Product I.D.")},
	{ 1, 60,IPTC_T_STR,   _T("Envelope Priority")},
	{ 1, 70,IPTC_T_STR,   _T("Date Sent")},
	{ 1, 80,IPTC_T_STR,   _T("Time Sent")},
	{ 1, 90,IPTC_T_STR,   _T("Coded Character Set")},
	{ 1,100,IPTC_T_STR,   _T("UNO")},
	{ 1,120,IPTC_T_NUM2,  _T("ARM Identifier")},
	{ 1,122,IPTC_T_NUM2,  _T("ARM Version")},
	{ 2,  0,IPTC_T_NUM2,  _T("Record Version")},
	{ 2,  3,IPTC_T_STR,   _T("Object Type Reference")},
	{ 2,  4,IPTC_T_STR,   _T("Object Attrib Reference")},
	{ 2,  5,IPTC_T_STR,   _T("Object Name")},
	{ 2,  7,IPTC_T_STR,   _T("Edit Status")},
	{ 2,  8,IPTC_T_STR,   _T("Editorial Update")},
	{ 2, 10,IPTC_T_STR,   _T("Urgency")},
	{ 2, 12,IPTC_T_STR,   _T("Subject Reference")},
	{ 2, 15,IPTC_T_STR,   _T("Category")},
	{ 2, 20,IPTC_T_STR,   _T("Supplemental Category")},
	{ 2, 22,IPTC_T_STR,   _T("Fixture Identifier")},
	{ 2, 25,IPTC_T_STR,   _T("Keywords")},
	{ 2, 26,IPTC_T_STR,   _T("Content Location Code")},
	{ 2, 27,IPTC_T_STR,   _T("Content Location Name")},
	{ 2, 30,IPTC_T_STR,   _T("Release Date")},
	{ 2, 35,IPTC_T_STR,   _T("Release Time")},
	{ 2, 37,IPTC_T_STR,   _T("Expiration Date")},
	{ 2, 38,IPTC_T_STR,   _T("Expiration Time")},
	{ 2, 40,IPTC_T_STR,   _T("Special Instructions")},
	{ 2, 42,IPTC_T_STR,   _T("Action Advised")},
	{ 2, 45,IPTC_T_UNK,   _T("Reference Service")},
	{ 2, 47,IPTC_T_UNK,   _T("Reference Date")},
	{ 2, 50,IPTC_T_UNK,   _T("Reference Number")},
	{ 2, 55,IPTC_T_STR,   _T("Date Created")},
	{ 2, 60,IPTC_T_STR,   _T("Time Created")},
	{ 2, 62,IPTC_T_STR,   _T("Digital Creation Date")},
	{ 2, 63,IPTC_T_STR,   _T("Digital Creation Time")},
	{ 2, 65,IPTC_T_STR,   _T("Originating Program")},
	{ 2, 70,IPTC_T_STR,   _T("Program Version")},
	{ 2, 75,IPTC_T_STR,   _T("Object Cycle")},
	{ 2, 80,IPTC_T_STR,   _T("By-line")},
	{ 2, 85,IPTC_T_STR,   _T("By-line Title")},
	{ 2, 90,IPTC_T_STR,   _T("City")},
	{ 2, 92,IPTC_T_STR,   _T("Sub-location")},
	{ 2, 95,IPTC_T_STR,   _T("Province/State")},
	{ 2,100,IPTC_T_STR,   _T("Country/Primary Location Code")},
	{ 2,101,IPTC_T_STR,   _T("Country/Primary Location Name")},
	{ 2,103,IPTC_T_STR,   _T("Original Transmission Reference")},
	{ 2,105,IPTC_T_STR,   _T("Headline")},
	{ 2,110,IPTC_T_STR,   _T("Credit")},
	{ 2,115,IPTC_T_STR,   _T("Source")},
	{ 2,116,IPTC_T_STR,   _T("Copyright Notice")},
	{ 2,118,IPTC_T_STR,   _T("Contact")},
	{ 2,120,IPTC_T_STR,   _T("Caption/Abstract")},
	{ 2,122,IPTC_T_STR,   _T("Writer/Editor")},
	{ 2,125,IPTC_T_UNK,   _T("Rasterized Caption")},
	{ 2,130,IPTC_T_STR,   _T("Image Type")},
	{ 2,131,IPTC_T_STR,   _T("Image Orientation")},
	{ 2,135,IPTC_T_STR,   _T("Language Identifier")},
	{ 2,150,IPTC_T_STR,   _T("Audio Type")},
	{ 2,151,IPTC_T_STR,   _T("Audio Sampling Rate")},
	{ 2,152,IPTC_T_STR,   _T("Audio Sampling Resolution")},
	{ 2,153,IPTC_T_STR,   _T("Audio Duration")},
	{ 2,154,IPTC_T_STR,   _T("Audio Outcue")},
	{ 2,200,IPTC_T_NUM2,  _T("ObjectData Preview File Format")},
	{ 2,201,IPTC_T_NUM2,  _T("ObjectData Preview File Format Version")},
	{ 2,202,IPTC_T_UNK,   _T("ObjectData Preview Data")},
	{ 7, 10,IPTC_T_NUM1,   _T("Size Mode")},
	{ 7, 20,IPTC_T_NUM,   _T("Max Subfile Size")},
	{ 7, 90,IPTC_T_NUM1,   _T("ObjectData Size Announced")},
	{ 7, 95,IPTC_T_NUM,   _T("Maximum ObjectData Size")},
	{ 8, 10,IPTC_T_UNK,   _T("Subfile")},
	{ 9, 10,IPTC_T_NUM,   _T("Confirmed ObjectData Size")},
	{ 0,  0,IPTC_T_END,   _T("DONE")},
};



// Adobe Photoshop File Formats Specification (October 2013)
// - Image Resource Blocks IRB (8BIM)
// - See header for struct encoding
struct tsBimRecord asBimRecords[] =
{
	{ 0x03E8, 0x0000, BIM_T_UNK,						_T("-")},
	{ 0x03E9, 0x0000, BIM_T_HEX,						_T("Macintosh print manager print info record")},
	{ 0x03EB, 0x0000, BIM_T_HEX,						_T("Indexed color table")},
	{ 0x03ED, 0x0000, BIM_T_PS_RESOLUTION_INFO,			_T("ResolutionInfo structure")},
	{ 0x03EE, 0x0000, BIM_T_HEX,						_T("Names of alpha channels")},
	{ 0x03EF, 0x0000, BIM_T_HEX,						_T("DisplayInfo structure")},
	{ 0x03F0, 0x0000, BIM_T_HEX,						_T("Caption")},
	{ 0x03F1, 0x0000, BIM_T_HEX,						_T("Border information")},
	{ 0x03F2, 0x0000, BIM_T_HEX,						_T("Background color")},
	{ 0x03F3, 0x0000, BIM_T_PS_PRINT_FLAGS,				_T("Print flags")},
	{ 0x03F4, 0x0000, BIM_T_HEX,						_T("Grayscale and multichannel halftoning information")},
	{ 0x03F5, 0x0000, BIM_T_HEX,						_T("Color halftoning information")},
	{ 0x03F6, 0x0000, BIM_T_HEX,						_T("Duotone halftoning information")},
	{ 0x03F7, 0x0000, BIM_T_HEX,						_T("Grayscale and multichannel transfer function")},
	{ 0x03F8, 0x0000, BIM_T_HEX,						_T("Color transfer functions")},
	{ 0x03F9, 0x0000, BIM_T_HEX,						_T("Duotone transfer functions")},
	{ 0x03FA, 0x0000, BIM_T_HEX,						_T("Duotone image information")},
	{ 0x03FC, 0x0000, BIM_T_HEX,						_T("-")},
	{ 0x03FD, 0x0000, BIM_T_HEX,						_T("EPS options")},
	{ 0x03FE, 0x0000, BIM_T_HEX,						_T("Quick Mask information")},
	{ 0x03FF, 0x0000, BIM_T_HEX,						_T("-")},
	{ 0x0400, 0x0000, BIM_T_PS_LAYER_STATE_INFO,		_T("Layer state information")},
	{ 0x0401, 0x0000, BIM_T_HEX,						_T("Working path")},
	{ 0x0402, 0x0000, BIM_T_PS_LAYER_GROUP_INFO,		_T("Layers group information")},
	{ 0x0403, 0x0000, BIM_T_HEX,						_T("-")},
	{ 0x0404, 0x0000, BIM_T_IPTC_NAA,					_T("IPTC-NAA record")},
	{ 0x0405, 0x0000, BIM_T_HEX,						_T("Image mode (raw format files)")},
	{ 0x0406, 0x0000, BIM_T_JPEG_QUAL,					_T("JPEG quality")},
	{ 0x0408, 0x0000, BIM_T_PS_GRID_GUIDES,				_T("Grid and guides information")},
	{ 0x0409, 0x0000, BIM_T_HEX,						_T("Thumbnail resource (PS 4.0)")},
	{ 0x040A, 0x0000, BIM_T_PS_COPYRIGHT_FLAG,			_T("Copyright flag")},
	{ 0x040B, 0x0000, BIM_T_HEX,						_T("URL")},
	{ 0x040C, 0x0000, BIM_T_PS_THUMB_RES,				_T("Thumbnail resources")},
	{ 0x040D, 0x0000, BIM_T_PS_GLOBAL_ANGLE,			_T("Global Angle")},
	{ 0x040E, 0x0000, BIM_T_HEX,						_T("Color samplers resource")},
	{ 0x040F, 0x0000, BIM_T_HEX,						_T("ICC Profile")},
	{ 0x0410, 0x0000, BIM_T_HEX,						_T("Watermark")},
	{ 0x0411, 0x0000, BIM_T_HEX,						_T("ICC Untagged Profile")},
	{ 0x0412, 0x0000, BIM_T_HEX,						_T("Effects visible")},
	{ 0x0413, 0x0000, BIM_T_HEX,						_T("Spot Halftone")},
	{ 0x0414, 0x0000, BIM_T_PS_DOC_SPECIFIC_SEED,		_T("Document-specific IDs seed number")},
	{ 0x0415, 0x0000, BIM_T_PS_STR_UNI,					_T("Unicode Alpha Names")},
	{ 0x0416, 0x0000, BIM_T_HEX,						_T("Indexed Color Table Count")},
	{ 0x0417, 0x0000, BIM_T_HEX,						_T("Transparency Index")},
	{ 0x0419, 0x0000, BIM_T_PS_GLOBAL_ALTITUDE,			_T("Global Altitude")},
	{ 0x041A, 0x0000, BIM_T_PS_SLICES,					_T("Slices")},
	{ 0x041B, 0x0000, BIM_T_PS_STR_UNI,					_T("Workflow URL")},
	{ 0x041C, 0x0000, BIM_T_HEX,						_T("Jump to XPEP")},
	{ 0x041D, 0x0000, BIM_T_HEX,						_T("Alpha Identifiers")},
	{ 0x041E, 0x0000, BIM_T_HEX,						_T("URL List")},
	{ 0x0421, 0x0000, BIM_T_PS_VER_INFO,				_T("Version Info")},
	{ 0x0422, 0x0000, BIM_T_HEX,						_T("Exif data 1")},
	{ 0x0423, 0x0000, BIM_T_HEX,						_T("Exif data 3")},
	{ 0x0424, 0x0000, BIM_T_PS_STR_ASC_LONG,			_T("XMP metadata")},
	{ 0x0425, 0x0000, BIM_T_HEX,						_T("Caption digest")},
	{ 0x0426, 0x0000, BIM_T_PS_PRINT_SCALE,				_T("Print scale")},
	{ 0x0428, 0x0000, BIM_T_PS_PIXEL_ASPECT_RATIO,		_T("Pixel Aspect Ratio")},
	{ 0x0429, 0x0000, BIM_T_HEX,						_T("Layer Comps")},
	{ 0x042A, 0x0000, BIM_T_HEX,						_T("Alternate Duotone Colors")},
	{ 0x042B, 0x0000, BIM_T_HEX,						_T("Alternate Spot Colors")},
	{ 0x042D, 0x0000, BIM_T_PS_LAYER_SELECT_ID,			_T("Layer Selection IDs")},
	{ 0x042E, 0x0000, BIM_T_HEX,						_T("HDR Toning information")},
	{ 0x042F, 0x0000, BIM_T_HEX,						_T("Print info")},
	{ 0x0430, 0x0000, BIM_T_PS_LAYER_GROUP_ENABLED,		_T("Layer Groups Enabled ID")},
	{ 0x0431, 0x0000, BIM_T_HEX,						_T("Color samplers resource")},
	{ 0x0432, 0x0000, BIM_T_HEX,						_T("Measurement Scale")},
	{ 0x0433, 0x0000, BIM_T_HEX,						_T("Timeline Information")},
	{ 0x0434, 0x0000, BIM_T_HEX,						_T("Sheet Disclosure")},
	{ 0x0435, 0x0000, BIM_T_HEX,						_T("DisplayInfo structure (FloatPt colors)")},
	{ 0x0436, 0x0000, BIM_T_HEX,						_T("Onion Skins")},
	{ 0x0438, 0x0000, BIM_T_HEX,						_T("Count Information")},
	{ 0x043A, 0x0000, BIM_T_HEX,						_T("Print Information")},
	{ 0x043B, 0x0000, BIM_T_HEX,						_T("Print Style")},
	{ 0x043C, 0x0000, BIM_T_HEX,						_T("Macintosh NSPrintInfo")},
	{ 0x043D, 0x0000, BIM_T_HEX,						_T("Windows DEVMODE")},
	{ 0x043E, 0x0000, BIM_T_HEX,						_T("Auto Save File Path")},
	{ 0x043F, 0x0000, BIM_T_HEX,						_T("Auto Save Format")},
	{ 0x0440, 0x0000, BIM_T_HEX,						_T("Path Selection State")},
	{ 0x07D0, 0x0BB6, BIM_T_HEX,						_T("Path Information (saved paths)")},
	{ 0x0BB7, 0x0000, BIM_T_HEX,						_T("Name of clipping path")},
	{ 0x0BB8, 0x0000, BIM_T_HEX,						_T("Origin Path Info")},
	{ 0x0FA0, 0x1387, BIM_T_HEX,						_T("Plug-In resources")},
	{ 0x1B58, 0x0000, BIM_T_HEX,						_T("Image Ready variables")},
	{ 0x1B59, 0x0000, BIM_T_HEX,						_T("Image Ready data sets")},
	{ 0x1F40, 0x0000, BIM_T_HEX,						_T("Lightroom workflow")},
	{ 0x2710, 0x0000, BIM_T_PS_PRINT_FLAGS_INFO,		_T("Print flags information")},
	{ 0x0000, 0x0000, BIM_T_END,						_T("")},
};


// Adobe Photoshop enumerated constants 
// - See header for struct encoding
struct tsBimEnum asBimEnums[] =
{
	{ BIM_T_ENUM_FILE_HDR_COL_MODE,				0,_T("Bitmap")},
	{ BIM_T_ENUM_FILE_HDR_COL_MODE,				1,_T("Grayscale")},
	{ BIM_T_ENUM_FILE_HDR_COL_MODE,				2,_T("Indexed")},
	{ BIM_T_ENUM_FILE_HDR_COL_MODE,				3,_T("RGB")},
	{ BIM_T_ENUM_FILE_HDR_COL_MODE,				4,_T("CMYK")},
	{ BIM_T_ENUM_FILE_HDR_COL_MODE,				7,_T("Multichannel")},
	{ BIM_T_ENUM_FILE_HDR_COL_MODE,				8,_T("Duotone")},
	{ BIM_T_ENUM_FILE_HDR_COL_MODE,				9,_T("Lab")},
	{ BIM_T_ENUM_RESOLUTION_INFO_RES_UNIT,		1,_T("pixels per inch")},
	{ BIM_T_ENUM_RESOLUTION_INFO_RES_UNIT,		2,_T("pixels per cm")},
	{ BIM_T_ENUM_RESOLUTION_INFO_WIDTH_UNIT,	1,_T("inch")},
	{ BIM_T_ENUM_RESOLUTION_INFO_WIDTH_UNIT,	2,_T("cm")},
	{ BIM_T_ENUM_RESOLUTION_INFO_WIDTH_UNIT,	3,_T("picas")},
	{ BIM_T_ENUM_RESOLUTION_INFO_WIDTH_UNIT,	4,_T("columns")},
	{ BIM_T_ENUM_PRINT_SCALE_STYLE,				0,_T("centered")},
	{ BIM_T_ENUM_PRINT_SCALE_STYLE,				1,_T("size to fit")},
	{ BIM_T_ENUM_PRINT_SCALE_STYLE,				2,_T("user defined")},
	{ BIM_T_ENUM_GRID_GUIDE_DIR,				0,_T("vertical")},
	{ BIM_T_ENUM_GRID_GUIDE_DIR,				1,_T("horizontal")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'pass',_T("pass through")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'norm',_T("normal")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'diss',_T("dissolve")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'dark',_T("darken")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'mul ',_T("multiply")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'idiv',_T("color burn")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'lbrn',_T("linear burn")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'dkCl',_T("darker color")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'lite',_T("lighten")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'scrn',_T("screen")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'div ',_T("color dodge")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'lddg',_T("linear dodge")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'lgCl',_T("lighter color")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'over',_T("overlay")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'sLit',_T("soft light")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'hLit',_T("hard light")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'vLit',_T("vivid light")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'lLit',_T("linear light")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'pLit',_T("pin light")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'hMix',_T("hard mix")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'diff',_T("difference")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'smud',_T("exclusion")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'fsub',_T("subtract")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'fdiv',_T("divide")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'hue ',_T("hue")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'sat ',_T("saturation")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'colr',_T("color")},
	{ BIM_T_ENUM_BLEND_MODE_KEY,				'lum ',_T("luminosity")},
	{ BIM_T_ENUM_END,							0,_T("???")},
};
