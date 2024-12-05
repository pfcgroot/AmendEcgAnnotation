// AmendEcgAnnotation.cpp : main entry point
// Fix dicom Waveform Annotations tags while archving from Muse to VNA.
// Compiled with Visual Studio (W10-64) and Red Hat linux 7 and 8.
//
// https://dicom.innolitics.com/ciods/12-lead-ecg/waveform-annotation
// 
// exit status:
//	0  = success
//	<0 = failure; source data not specified/accessible or destination write failure
//	>0 = warning
//
// (c) 2021 Amsterdam UMC - Dept of Radiology and Nuclear Medicine - Paul F.C. Groot

#include "AmendEcgAnnotation.h"

#include "dcmtk/config/osconfig.h"    /* make sure OS specific configuration is included first */
#include "dcmtk/ofstd/ofstream.h"
#include "dcmtk/dcmdata/dctk.h"
#include "dcmtk/dcmdata/cmdlnarg.h"
#include "dcmtk/ofstd/ofconapp.h"
#include "dcmtk/ofstd/ofstd.h"
#include "dcmtk/ofstd/ofstack.h"
#include "dcmtk/ofstd/ofstrutl.h"

#define MY_NAME "AmendEcgAnnotation"
#define MY_VERSION "0.9.3"
#define MY_RELEASEDATE "2021-10-13"

#define RESULT_SUCCESS 0
#define RESULT_FAILED_TO_CREATE -1
#define RESULT_FAILED_TO_READ -2
#define RESULT_ERROR_MISSING_TAG -3
#define RESULT_ERROR_WRONGSOP_CLASS -4
#define RESULT_WARN_NO_CHANGES 1
#define RESULT_WARN_ALREADY_AMENDED 2
#define RESULT_FAILED_TO_CLONE_OFFSET 10 // either pos or neg, depending on result code of error/warning

static const char rcsid[]	= "$amc.nl: " MY_NAME " v" MY_VERSION " " MY_RELEASEDATE " $";
static OFBool _bVerbose = OFFalse;
static OFBool _bforceOutput = OFFalse;
static OFBool _bNoCloneOnErrror = OFFalse;
static OFBool _bMergeLines = OFFalse;
//static OFBool _bRetrospectiveConversion = OFFalse;
static OFBool _bIncludeBevestigingDoor = OFTrue; // set to false when 'Bevestigd' is found in one of the text annotations

#define SHORTCOL 3
#define LONGCOL 20

using namespace std;

// used tags (info)
static const DcmTagKey _tagPatientID(DCM_PatientID);
static const DcmTagKey _tagAccessionNumber(DCM_AccessionNumber);
static const DcmTagKey _tagStudyDescription(DCM_StudyDescription);							// (0008,1030)
static const DcmTagKey _tagSOPClassUID(DCM_SOPClassUID);									// (0008,0016)
static const DcmTagKey _tagWaveformSequence(DCM_WaveformSequence);							// (5400,0100) SQ each item represents one waveform multiplex group
static const DcmTagKey _tagWaveformOriginality(DCM_WaveformOriginality);					// (003a,0004) CS 
static const DcmTagKey _tagNumberOfWaveformChannels(DCM_NumberOfWaveformChannels);			// (003a,0005) US 
// used tags (sources)
static const DcmTagKey _tagVisitComments(DCM_VisitComments);								// (0038,4000) LT
static const DcmTagKey _tagOperatorsName(DCM_OperatorsName);								// (0008,1070) PN (multiplicity 1-n)
static const DcmTagKey _tagReferringPhysicianName(DCM_ReferringPhysicianName);				// (0008,0090) PN 
static const DcmTagKey _tagPhysiciansOfRecord(DCM_PhysiciansOfRecord);						// (0008,1048) PN (multiplicity 1-n)
static const DcmTagKey _tagNameOfPhysiciansReadingStudy(DCM_NameOfPhysiciansReadingStudy);	// (0008,1060) PN (multiplicity 1-n)
// used tags (destination)
static const DcmTagKey _tagWaveformAnnotationSequence(DCM_WaveformAnnotationSequence);		// (0040,b020) SQ
static const DcmTagKey _tagUnformattedTextValue(DCM_UnformattedTextValue);					// (0070,0006) ST
static const DcmTagKey _tagReferencedWaveformChannels(DCM_ReferencedWaveformChannels);		// (0040,a0b0) US 1\0 (multiplicity 2-2n)
static const DcmTagKey _tagAnnotationGroupNumber(DCM_AnnotationGroupNumber);				// (0040,a180) US

static const OFString _ofstrAnnotationSeparator("-+-");

// clone input to output in case of an error
static int TryFileClone(const OFFilename& ofstrInputFile, const OFFilename& ofstrOutputFile, int resultSoFar)
{
	if (_bNoCloneOnErrror && resultSoFar<0)
		return resultSoFar;

	if (OFStandard::fileExists(ofstrInputFile))
	{
		if (0==strcmp(ofstrInputFile.getCharPointer(), ofstrOutputFile.getCharPointer()))
			return resultSoFar; // no need to copy on itself

		if (OFStandard::copyFile(ofstrInputFile, ofstrOutputFile))
		{
			if (_bVerbose)
				COUT << "Cloned " << ofstrOutputFile;
			return resultSoFar;
		}
		else
		{
			CERR << "ERROR: could not clone " << ofstrOutputFile;
			return (resultSoFar<0 ? resultSoFar-RESULT_FAILED_TO_CLONE_OFFSET: resultSoFar+RESULT_FAILED_TO_CLONE_OFFSET);
		}
	}
	return RESULT_FAILED_TO_READ - RESULT_FAILED_TO_CLONE_OFFSET;
}

static OFString& HumanReadableName(OFString& s)
{
	// removes carets from DICOM PN
	static const OFString _ofstrCaret3("^^^");
	static const OFString _ofstrCaret2("^^");
	static const OFString _ofstrCaret("^");
	static const OFString _ofstrCommaSpace(", ");
	OFStringUtil::replace_all(s, _ofstrCaret3, _ofstrCaret);
	OFStringUtil::replace_all(s, _ofstrCaret2, _ofstrCaret);
	OFStringUtil::replace_all(s, _ofstrCaret2, _ofstrCaret);
	if (*s.begin() == '^') s.erase(0, 1);
	if (*(s.end()-1) == '^') s.erase(s.length()-1, 1);
	OFStringUtil::replace_all(s, _ofstrCaret, _ofstrCommaSpace);
	return s;
}

int main(int argc, char* argv[])
{
	OFConsoleApplication app(MY_NAME, "Ammend ECG Waveform annotation by copying VisitComments", rcsid);
	OFCommandLine cmd;
	OFFilename ofstrInputFile;
	OFFilename ofstrOutputFile;
	E_FileReadMode readMode = ERM_autoDetect;
	E_TransferSyntax xfer = EXS_Unknown;
	OFCmdUnsignedInt maxReadLength = 128; // default is 128 bytes
	DcmFileFormat dfile;
	OFCondition cond;

	cmd.setOptionColumns(LONGCOL, SHORTCOL);
	cmd.setParamColumn(LONGCOL + SHORTCOL + 4);

	cmd.addParam("dcmfile-in",  "DICOM input filename to be converted", OFCmdParam::PM_Mandatory);
	cmd.addParam("dcmfile-out", "DICOM output filename (default: dcmfile-in)", OFCmdParam::PM_Optional);

	cmd.addGroup("general options:", LONGCOL, SHORTCOL + 2);
	cmd.addOption("--help", "-h", "print this help text and exit");
	cmd.addOption("--version", "print version information and exit", OFTrue /* exclusive */);
	cmd.addOption("--verbose", "-v", "verbose mode, print processing details");

	cmd.addGroup("output options:");
	//cmd.addSubGroup("filesystem options:");
	cmd.addOption("--force", "-f", "overwrite existing file");
	cmd.addOption("--no-clone", "-n", "don't try to create clone on errors");
	cmd.addOption("--merge-lines", "-m", "merge amended lines into one paragraph");
	cmd.addOption("--retrospective-conversion", "-r", "retrospective (offline) conversion");


	/* evaluate command line */
	prepareCmdLineArgs(argc, argv, MY_NAME);

#ifdef HAVE_WINDOWS_H
#if OFFIS_DCMTK_VERSION_NUMBER>354
	const int CLflags = OFCommandLine::PF_ExpandWildcards;
#else
	const int CLflags = OFCommandLine::ExpandWildcards;
#endif
#else
	const int CLflags = 0;
#endif
	if (app.parseCommandLine(cmd, argc, argv, CLflags))
	{
		/* check exclusive options first */

		if (cmd.getParamCount() == 0)
		{
			app.printHeader(OFTrue /*print host identifier*/);
			app.printUsage(&cmd);
			return 0;
		}

		/* options */
		if (cmd.findOption("--version"))
			app.printHeader(OFTrue /*print host identifier*/); 

		if (cmd.findOption("--help"))
			app.printUsage(&cmd);

		if (cmd.findOption("--verbose"))
			_bVerbose = OFTrue;

		if (cmd.findOption("--force"))
			_bforceOutput = OFTrue;

		if (cmd.findOption("--no-clone"))
			_bNoCloneOnErrror = OFTrue;

		if (cmd.findOption("--merge-lines"))
			_bMergeLines = OFTrue;

//		if (cmd.findOption("--retrospective-conversion"))
//			_bRetrospectiveConversion = OFTrue;
	}

	// make sure data dictionary is loaded
	if (!dcmDataDict.isDictionaryLoaded())
	{
		CERR << "ERROR: no data dictionary loaded;  check environment variable: " << DCM_DICT_ENVIRONMENT_VARIABLE << std::endl;
		return RESULT_FAILED_TO_CREATE;
	}

	// loop through all arguments (i.e. input paths)
	const int nArgs = cmd.getParamCount();
	for (int iArg = 0; iArg < nArgs; iArg++)
	{
		OFFilename ofFilename;
		if (cmd.getParam(iArg + 1, ofFilename) == OFCommandLine::E_ParamValueStatus::PVS_Normal)
		{
			switch (iArg)
			{
			case 0: ofstrInputFile = ofFilename; break;
			case 1: ofstrOutputFile = ofFilename; break;
			default: assert(false);
			}
		}
	}

	if (ofstrOutputFile.isEmpty())
	{
		ofstrOutputFile = ofstrInputFile; 
		// requires --force
		if (!_bforceOutput)
		{
			CERR << "ERROR: Use --force to overwrite the original file, or specify an output file.";
			return RESULT_FAILED_TO_CREATE;
		}
	}
	else if (!_bforceOutput && OFStandard::fileExists(ofstrOutputFile))
	{
		CERR << "ERROR: Output file exists; use --force to overwrite.";
		return RESULT_FAILED_TO_CREATE;
	}

	if (_bVerbose)
	{
		COUT << "inp: " << ofstrInputFile << std::endl;
		COUT << "out: " << ofstrOutputFile << std::endl;
	}

	cond = dfile.loadFile(ofstrInputFile, xfer, EGL_noChange, maxReadLength, readMode);
	if (cond.bad())
	{
		CERR << "ERROR: could not load dicom file: " << cond.text() << endl;
		return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_FAILED_TO_READ);
	}
	if (dfile.loadAllDataIntoMemory().good())
	{
		DcmDataset* pDataset = dfile.getDataset();
		//(0008,0016) UI =TwelveLeadECGWaveformStorage            #  30, 1 SOPClassUID
		OFString ofstrValue;
		OFStack<OFString> ofstrStack; // stack with all lines to be added to the wave form annotation sequence
		char bufST[1024]; // maxumum number of characters allowed in VR=ST

		// first collect all relevant text items 

		OFString ofstrSOPClassUID;
		if (pDataset->findAndGetOFString(_tagSOPClassUID, ofstrSOPClassUID).good() && !ofstrSOPClassUID.empty())
		{
			if (_bVerbose)
				COUT << "INFO: SOPClassUID: " << ofstrSOPClassUID << endl;
		}
		else
		{
			if (_bVerbose)
				CERR << "WARN: SOPClassUID is missing or empty" << endl;
		}
		if (   ofstrSOPClassUID.compare("1.2.840.10008.5.1.4.1.1.9.1.1")!=0 
			&& ofstrSOPClassUID.compare("1.2.840.10008.5.1.4.1.1.9.1.2")!=0 
			&& ofstrSOPClassUID.compare("1.2.840.10008.5.1.4.1.1.9.1.3")!=0)
		{
			CERR << "ERROR: SOP class is not 12-lead, general or ambulatory ECG" << endl;
			return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_ERROR_WRONGSOP_CLASS);
		}

		if (_bVerbose)
		{
			OFString ofstrPatientID;
			if (pDataset->findAndGetOFString(_tagPatientID, ofstrPatientID).good())
				COUT << "INFO: PatientID: " << ofstrPatientID << endl;
			else
				CERR << "WARN: PatientID is missing" << endl;

			OFString ofstrAccessionNumber;
			if (pDataset->findAndGetOFString(_tagAccessionNumber, ofstrAccessionNumber).good())
				COUT << "INFO: AccessionNumber: " << ofstrAccessionNumber << endl;
			else
				CERR << "WARN: AccessionNumber is missing" << endl;
		}

		OFString ofstrStudyDescription;
		if (pDataset->findAndGetOFString(_tagStudyDescription, ofstrStudyDescription).good() && !ofstrStudyDescription.empty())
		{
			if (_bVerbose)
				COUT << "INFO: StudyDescription: " << ofstrStudyDescription << endl;
		}
		else
		{
			if (_bVerbose)
				CERR << "WARN: StudyDescription is missing or empty" << endl;
		}

		OFString ofstrVisitComments;
		if (pDataset->findAndGetOFString(_tagVisitComments, ofstrVisitComments).good() && !ofstrVisitComments.empty())
		{
			if (_bVerbose)
				COUT << "INFO: VisitComments: " << ofstrVisitComments.c_str() << endl;
			snprintf(bufST, sizeof(bufST)/sizeof(bufST[0]), "Testind: %s", ofstrVisitComments.c_str());
			ofstrVisitComments = bufST;
		}
		else
		{
			if (_bVerbose)
				CERR << "WARN: VisitComments is missing or empty" << endl;
		}

		OFString ofstrOperatorsName;
		if (pDataset->findAndGetOFStringArray(_tagOperatorsName, ofstrOperatorsName).good() && !ofstrOperatorsName.empty())
		{
			if (_bVerbose)
				COUT << "INFO: OperatorsName: " << ofstrOperatorsName.c_str() << endl;
			HumanReadableName(ofstrOperatorsName);
			snprintf(bufST, sizeof(bufST) / sizeof(bufST[0]), "Technicus: %s", ofstrOperatorsName.c_str());
			ofstrOperatorsName = bufST;
		}
		else
		{
			if (_bVerbose)
				CERR << "WARN: OperatorsName is missing or empty" << endl;
		}

		OFString ofstrReferringPhysicianName;
		if (pDataset->findAndGetOFString(_tagReferringPhysicianName, ofstrReferringPhysicianName).good() && !ofstrReferringPhysicianName.empty())
		{
			if (_bVerbose)
				COUT << "INFO: ReferringPhysicianName: " << ofstrReferringPhysicianName.c_str() << endl;
			HumanReadableName(ofstrReferringPhysicianName);
			snprintf(bufST, sizeof(bufST) / sizeof(bufST[0]), "Verwezen door: %s", ofstrReferringPhysicianName.c_str());
			ofstrReferringPhysicianName = bufST;
		}
		else
		{
			if (_bVerbose)
				CERR << "WARN: ReferringPhysicianName is missing or empty" << endl;
		}

		OFString ofstrPhysiciansOfRecord;
		if (pDataset->findAndGetOFStringArray(_tagPhysiciansOfRecord, ofstrPhysiciansOfRecord).good() && !ofstrPhysiciansOfRecord.empty())
		{
			if (_bVerbose)
				COUT << "INFO: PhysiciansOfRecord : " << ofstrPhysiciansOfRecord.c_str() << endl;
			HumanReadableName(ofstrPhysiciansOfRecord);
			snprintf(bufST, sizeof(bufST) / sizeof(bufST[0]), "Aangevraagd door: %s", ofstrPhysiciansOfRecord.c_str());
			ofstrPhysiciansOfRecord = bufST;
		}
		else
		{
			if (_bVerbose)
				CERR << "WARN: PhysiciansOfRecord  is missing or empty" << endl;
		}

		OFString ofstrNameOfPhysiciansReadingStudy;
		if (pDataset->findAndGetOFStringArray(_tagNameOfPhysiciansReadingStudy, ofstrNameOfPhysiciansReadingStudy).good() && !ofstrNameOfPhysiciansReadingStudy.empty())
		{
			if (_bVerbose)
				COUT << "INFO: NameOfPhysiciansReadingStudy : " << ofstrNameOfPhysiciansReadingStudy.c_str() << endl;

			// only add this name in restropective mode; not in the live stream because it will already be handled in that case
			if (_bIncludeBevestigingDoor)
			{
				HumanReadableName(ofstrNameOfPhysiciansReadingStudy);
				snprintf(bufST, sizeof(bufST) / sizeof(bufST[0]), "Bevestigd door: %s", ofstrNameOfPhysiciansReadingStudy.c_str());
				ofstrNameOfPhysiciansReadingStudy = bufST;
			}
			else
				ofstrNameOfPhysiciansReadingStudy.clear();
		}
		else
		{
			if (_bVerbose)
				CERR << "WARN: NameOfPhysiciansReadingStudy  is missing or empty" << endl;

			ofstrNameOfPhysiciansReadingStudy="Bevestigd door: Onbevestigd";
		}

		// add text items to stack; start with a separator
		/*always*/											ofstrStack.push(_ofstrAnnotationSeparator);
		if (!ofstrVisitComments.empty())					ofstrStack.push(ofstrVisitComments);
		if (!ofstrOperatorsName.empty())					ofstrStack.push(ofstrOperatorsName);
		if (!ofstrReferringPhysicianName.empty())			ofstrStack.push(ofstrReferringPhysicianName);
		if (!ofstrPhysiciansOfRecord.empty())				ofstrStack.push(ofstrPhysiciansOfRecord);
		if (!ofstrNameOfPhysiciansReadingStudy.empty())		ofstrStack.push(ofstrNameOfPhysiciansReadingStudy);

		// stop here if there is nothing to add
		if (ofstrStack.size()<=1) // first item is a dummy separator
		{
			CERR << "WARN: All source tags are missing; skipping" << endl;
			return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_WARN_NO_CHANGES);
		}

		OFString ofstrMerged;
		if (_bMergeLines)
		{
			if (_bVerbose)
				COUT << "INFO: Merging lines into paragraph" << endl;

			for (; !ofstrStack.empty(); ofstrStack.pop())
			{
				const OFString& strLine = ofstrStack.top();
				if (ofstrMerged.empty())
					ofstrMerged = strLine;
				else
					ofstrMerged = strLine + "\r\n" + ofstrMerged;
			}
			ofstrStack.push(ofstrMerged);
		}

		// get a reference to the Waveform Sequence; this is just to make sure we have waveforms
		DcmSequenceOfItems* seqWaveform = NULL;
		OFString ofstrReferencedWaveformChannels;
		unsigned long ulNumberOfMultiplexWaveforms = 0;
		if (pDataset->findAndGetSequence(_tagWaveformSequence, seqWaveform).good())
		{
			ulNumberOfMultiplexWaveforms = seqWaveform->card();
			unsigned long iMultiplexWaveform = 0;
			for (iMultiplexWaveform = 0; iMultiplexWaveform < ulNumberOfMultiplexWaveforms; iMultiplexWaveform++)
			{
				DcmItem* pItem = seqWaveform->getItem(iMultiplexWaveform);
				if (_bVerbose && pItem)
				{
					OFString ofstdWaveformOriginality;
					Uint16 nChannels;

					pItem->findAndGetOFString(_tagWaveformOriginality, ofstdWaveformOriginality);
					pItem->findAndGetUint16(_tagNumberOfWaveformChannels, nChannels);

					if (ofstrReferencedWaveformChannels.empty() && ofstdWaveformOriginality.compare("ORIGINAL")==0)
					{
						snprintf(bufST, sizeof(bufST) / sizeof(bufST[0]), "%lu\\0", iMultiplexWaveform+1);
						ofstrReferencedWaveformChannels = bufST;
					}

					COUT << "INFO: MultiplexWaveform [" << iMultiplexWaveform << "] = " << ofstdWaveformOriginality << ", N=" << nChannels << endl;
				}
			}
		}
		if (ulNumberOfMultiplexWaveforms == 0)
		{
			CERR << "ERROR: No waveform multiplex sequence." << endl;
			return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_ERROR_MISSING_TAG);
		}

		// get a reference to the Waveform Annotation Sequence and locate the last item with an UnformattedTextValue tag
		DcmSequenceOfItems* seqWaveformAnnotations = NULL;
		DcmItem* pLastUnformattedTextItem = NULL;
		unsigned long iFirstNonTextItem = DCM_EndOfListIndex; // this will be the item to insert at/before
		if (pDataset->findAndGetSequence(_tagWaveformAnnotationSequence, seqWaveformAnnotations).good())
		{
			// Example ofstrUnformattedTextValue annotation Item
			// (fffe, e000) na(Item with undefined length # = 3)         # u / l, 1 Item
			//    (0040, a0b0) US 1\0                                      #   4, 2 ReferencedWaveformChannels
			//    (0040, a180) US 0                                        #   2, 1 AnnotationGroupNumber
			//    (0070, 0006) ST[Sinusbradycardie Met 1e graads av - block Met incidentele Ventricula... #  84, 1 UnformattedTextValue
			// (fffe, e00d) na(ItemDelimitationItem)                   #   0, 0 ItemDelimitationItem

			OFString ofstrValue;
			for (unsigned long iItem = 0; iItem < seqWaveformAnnotations->card(); iItem++)
			{
				if (_bVerbose)
					COUT << "INFO: Item: " << iItem << endl;
				DcmItem* pItem = seqWaveformAnnotations->getItem(iItem);

				// check for an UnformattedTextValue
				if (pItem->findAndGetOFString(_tagUnformattedTextValue, ofstrValue).good())
				{
					if (_bVerbose)
						COUT << "INFO: Found UnformattedTextValue: " << ofstrValue << endl;

					//std::transform(ofstrValue.begin(), ofstrValue.end(), ofstrValue.begin(), ::toupper);
					if (ofstrValue.find(_ofstrAnnotationSeparator) != string::npos)
					{
						CERR << "WARN: Waveform annotation already amended; skipping" << endl;
						return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_WARN_ALREADY_AMENDED);
					}
					else if (ofstrValue.find("evestigd") != string::npos) // Ignore capital B from Bevestigd in case it might be lower case
					{
						if (_bVerbose)
							COUT << "INFO: Bevestiging al ingevoerd; skip this item in amendment" << endl;
						_bIncludeBevestigingDoor = OFFalse;
					}
					pLastUnformattedTextItem = pItem;
				}
				else
				{
					// if UnformattedTextValue is missing, sremember to start insertions of new item here
					if (iFirstNonTextItem==DCM_EndOfListIndex)
						iFirstNonTextItem = iItem;
				}

				if (_bVerbose)
				{
					// ReferencedWaveformChannels
					if (pItem->findAndGetOFStringArray(_tagReferencedWaveformChannels, ofstrValue).good())
						COUT << "      ReferencedWaveformChannels: " << ofstrValue << endl;

					// AnnotationGroupNumber
					if (pItem->findAndGetOFString(_tagAnnotationGroupNumber, ofstrValue).good())
						COUT << "      AnnotationGroupNumber: " << ofstrValue << endl;
				}
			}
		}
		if (!seqWaveformAnnotations)
		{
			DcmElement* pNewItem = pDataset->newDicomElement(_tagWaveformAnnotationSequence);
			if (!pNewItem)
			{
				CERR << "FAIL: Failed to create new WaveformAnnotationSequence" << endl;
				return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_FAILED_TO_CREATE);
			}
			else
			{
				if (_bVerbose)
					COUT << "INFO: added new WaveformAnnotationSequence" << endl;
				seqWaveformAnnotations = OFstatic_cast(DcmSequenceOfItems*, pNewItem);
			}
		}
		// create a dummy annotation, even if there was nothing defined yet
		if (pLastUnformattedTextItem == NULL)
		{
			if (ofstrReferencedWaveformChannels.empty())
			{
				COUT << "WARN: ORIGINAL Waveform multiplex group not found; assuming group 1" << endl;
				ofstrReferencedWaveformChannels = "1\\0";
			}
			if (_bVerbose)
				COUT << "INFO: creating a dummy annotation." << endl;
			pLastUnformattedTextItem = new DcmItem;
			if (pLastUnformattedTextItem->putAndInsertString(_tagReferencedWaveformChannels, ofstrReferencedWaveformChannels.c_str()).bad()) // putAndInsertStringArray fails
			{
				CERR << "FAIL: Failed to add ReferencedWaveformChannels to dummy: " << ofstrReferencedWaveformChannels << endl;
				return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_FAILED_TO_CREATE);
			}
			if (pLastUnformattedTextItem->putAndInsertString(_tagAnnotationGroupNumber, "0").bad())
			{
				CERR << "FAIL: Failed to add AnnotationGroupNumber to dummy: 0" << endl;
				return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_FAILED_TO_CREATE);
			}
		}

		if (pLastUnformattedTextItem!=NULL)
		{
			for (;!ofstrStack.empty(); ofstrStack.pop())
			{
				const OFString& strLine = ofstrStack.top();
				DcmItem* pNew = OFstatic_cast(DcmItem*, pLastUnformattedTextItem->clone());
				// UnformattedTextValue
				if (pNew->putAndInsertString(_tagUnformattedTextValue, strLine.c_str()).good())
				{
					// and insert it after the previous text annotation
					seqWaveformAnnotations->insert(pNew, iFirstNonTextItem, true);
					if (_bVerbose)
					{
						unsigned long ulInsertAt = iFirstNonTextItem == DCM_EndOfListIndex ? 0 : iFirstNonTextItem;
						COUT << "INFO: Inserting new annotation: [" << ulInsertAt << "] = " << strLine << endl;
					}
				}
				else
				{
					CERR << "ERROR: missing tag UnformattedTextValue" << endl;
					return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_ERROR_MISSING_TAG);
				}
			}
		}
		else
		{
			CERR << "FAIL: failed to create dummy Waveform annotation" << endl;
			return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_FAILED_TO_CREATE);
		}

		if (dfile.saveFile(ofstrOutputFile, pDataset->getOriginalXfer(), EET_UndefinedLength, EGL_recalcGL, EPD_noChange, 0, 0, EWM_createNewMeta).good())
		{
			if (_bVerbose)
				COUT << "INFO: Created output file: " << ofstrOutputFile << endl;
		}
		else
		{
			CERR << "FAIL: failed to create output file: " << ofstrOutputFile << endl;
			return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_FAILED_TO_CREATE);
		}
	}
	else
	{
		CERR << "ERROR: failed reading all data" << endl;
		return TryFileClone(ofstrInputFile, ofstrOutputFile, RESULT_FAILED_TO_READ);
	}

	return RESULT_SUCCESS;
}
