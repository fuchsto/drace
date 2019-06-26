import xml.etree.ElementTree as ET
import os
import shutil
import argparse

#Paths
g_HTMLTEMPLATES = "templates/entries.xml"
g_CSSPATH = "templates/css"
g_JSPATH = "templates/js"
DEBUG = True

SOURCEFILE_BL = list()
SOURCEFILE_WL = list()
BLACKLISTING = True
WHITELISTING = False
NUMBEROFCODELINES = 500

class ReportCreator:
    __htmlTemplatesPath = g_HTMLTEMPLATES
    __htmlTemplates = (ET.parse(__htmlTemplatesPath)).getroot()
    
    def __init__(self, pathOfReport):
        self.sourcefileList = list()
        self.__callStackNumber = 0
        self.__errorNumber = 0
        self.__snippets = str()
        self.succesfullReportCreation = True

        self.__pathOfReport = pathOfReport
        if self.__inputValidation():
            self.__createReport()
        else:
            print("input file is not valid")
            self.succesfullReportCreation = False

    def __inputValidation(self):
        try:
            self.__reportContent = ET.parse(self.__pathOfReport)
            self.__reportRoot = self.__reportContent.getroot()
        except:
            return 0

        if self.__reportRoot.find('protocolversion') != None and \
            self.__reportRoot.find('protocoltool') != None and \
            self.__reportRoot.find('preamble') != None and \
            self.__reportRoot.find('pid') != None and \
            self.__reportRoot.find('tool') != None and \
            self.__reportRoot.find('args') != None and \
            self.__reportRoot.find('status') != None and \
            self.__reportRoot.tag == 'valgrindoutput':

            return 1
        else:
            return 0

    def __getHeader(self):
        header = list()
        status = self.__reportRoot.findall('status')[1]
        strDatetime = status.find('time').text
        date = strDatetime.split('T')[0]
        time = (strDatetime.split('T')[1])[0:-1] #last digit is 'Z' -> not needed
        header.append(self.adjText(date))
        header.append(self.adjText(time))
        header.append(self.adjText(status.find('duration').text))

        arguments = str()
        for arg in self.__reportRoot.find('args').find('vargv').findall('arg'):
            arguments += arg.text
            arguments += ' '
        header.append(self.adjText(arguments[0:-1])) #remove last ' '
        
        header.append(self.adjText(self.__reportRoot.find('args').find('argv').find('exe').text))
        header.append(self.adjText(self.__reportRoot.find('protocolversion').text))
        header.append(self.adjText(self.__reportRoot.find('protocoltool').text))

        return header

    def __handleSourceCode(self, filename, directory):
        fullPath = directory +'/'+ filename
        fullPath = fullPath.replace('\\', '/')
        
        src = self.__returnCode(fullPath, justExistance=1)
        if src == -1:
            return -1, self.__htmlTemplates.find('no_code_entry').text

        else:
            try:
                index = self.sourcefileList.index(fullPath)
            except ValueError:
                self.sourcefileList.append(fullPath)
                index = self.sourcefileList.index(fullPath)
            
            return index, (self.__htmlTemplates.find('code_entry').text)

    def __createCodeVars(self):
        codeString = str()

        for sourcefile in self.sourcefileList:
            src = self.__returnCode(sourcefile, justExistance=0)
            tmpCode = "code_" + str(self.sourcefileList.index(sourcefile)) + ' = `' + src + '`;\n'
            codeString += tmpCode

        self.htmlReport = self.htmlReport.replace("*CODE_VARIABLES*", codeString)
        self.htmlReport = self.htmlReport.replace("*SNIPPET_VARIABLES*", self.__snippets)

    def __returnCode(self, fullPath, justExistance):
        
        if os.path.isfile(fullPath):
            if BLACKLISTING:
                for element in SOURCEFILE_BL:
                    if element in fullPath:
                        return -1
                if justExistance:
                    return 0
                else:
                    sourceFile = open(fullPath, mode='r')
                    sourceCode = sourceFile.read()
                    sourceFile.close()
                    sourceCode = self.adjText(sourceCode)
                    return sourceCode

            if WHITELISTING:
                for element in SOURCEFILE_WL:
                    if element in fullPath:
                        if justExistance:
                            return 0
                    else:
                        sourceFile = open(fullPath, mode='r')
                        sourceCode = sourceFile.read()
                        sourceFile.close()
                        sourceCode = self.adjText(sourceCode)
                        return sourceCode
                return -1
        else:
            return -1

    def __determineLanguage(self, filename):
        fileParts = filename.split('.')
        if len(fileParts) == 1:
            return 'cpp' #files without file endigs are treated as cpp files
        else:
            ending = fileParts[-1]
            if ending == 'c':
                return 'c'
            elif ending == 'cpp':
                return 'cpp'
            elif ending == 'h':
                return 'cpp'
            elif ending == 'cs':
                return 'csharp'
            elif ending == 'css':
                return 'css'
            elif ending == 'js':
                return 'javascript'
            elif ending == 'html':
                return 'markup'
            else:
                return 'cpp'     

    def __createSnippetEntry(self, frame, elementNumber, tag, codeIndex):
        newSnippet = self.__htmlTemplates.find('snippet_entry').text

        newSnippet = newSnippet.replace('*SNIPPET_VAR*', ("snippet_" + str(self.__callStackNumber)))
        newSnippet = newSnippet.replace('*STACK_NUMBER*', self.adjText(hex(elementNumber)))
        newSnippet = newSnippet.replace('*OBJ*', self.adjText(frame.find('obj').text))
        newSnippet = newSnippet.replace('*FUNCTION*', self.adjText(frame.find('fn').text))
        newSnippet = newSnippet.replace('*INSTRUCTION_POINTER*', self.adjText(frame.find('ip').text))
        newSnippet = newSnippet.replace('*CODE_TAG*', tag)

        if (frame.find('file')!= None):
            newSnippet = newSnippet.replace('*FILE*', self.adjText(frame.find('file').text))
            newSnippet = newSnippet.replace('*DIRECTORY*', self.adjText(frame.find('dir').text))
            newSnippet = newSnippet.replace('*LINE_OF_CODE*', self.adjText(frame.find('line').text))
            if(codeIndex != -1):
                newSnippet = newSnippet.replace('*CODE_ID_VAR*', "snippet_"+str(self.__callStackNumber)+"_code")
                newSnippet = newSnippet.replace('*LANGUAGE*', self.__determineLanguage(self.adjText(frame.find('file').text)))
            else:
                newSnippet = newSnippet.replace('*CODE_ID_VAR*',  "'None'")

        else:
            newSnippet = newSnippet.replace('*FILE*', 'no filename available')
            newSnippet = newSnippet.replace('*DIRECTORY*', 'no directory available')
            newSnippet = newSnippet.replace('*LINE_OF_CODE*', 'no line of code available')

        self.__snippets += newSnippet #append referenced code snippet

    def __createCallStack(self, errorEntry, position, outputID):
        
        callStack = str()
        stackTemplate = self.__htmlTemplates.find('stack_entry').text
        stackArray = errorEntry.findall('stack')
        stack = stackArray[position]
        elementNumber = 0

        for frame in stack.findall('frame'):
            noPreview = False    
            ###make heading for the red box### 
            if elementNumber == 0:
                if len(self.__errorHeading) == 0:
                    self.__errorHeading += "<br> Obj. 1: " + (self.adjText(frame.find('obj').text) + ': "' + self.adjText(frame.find('fn').text)) + '" <br> '
                else:
                    self.__errorHeading += "Obj. 2: " + (self.adjText(frame.find('obj').text) + ': "' + self.adjText(frame.find('fn').text)) + '"'

            #general entries (always available)
            newStackElement = stackTemplate.replace('*STACK_NUMBER*', self.adjText(hex(elementNumber))+":")
            newStackElement = newStackElement.replace('*SNIPPET_VAR*', ("snippet_" + str(self.__callStackNumber)))
            newStackElement = newStackElement.replace('*OUTPUT_ID*', outputID+str(position))
            newStackElement = newStackElement.replace('*FUNCTION*', self.adjText(frame.find('fn').text))
            

            if (frame.find('file')!= None): #file is in xml report defined
                codeIndex, tag = self.__handleSourceCode(frame.find('file').text, frame.find('dir').text)
                newStackElement = newStackElement.replace('*FILE*', self.adjText(frame.find('file').text))

                if(codeIndex != -1):
                    newStackElement = newStackElement.replace('*CODE_VAR*', "code_"+str(codeIndex))
                    newStackElement = newStackElement.replace('*CODE_ID_VAR*', "'snippet_"+str(self.__callStackNumber)+"_code'")
                    newStackElement = newStackElement.replace('*LINE_OF_CODE*', self.adjText(frame.find('line').text))      
                
                else: #file is not available on device or file is blacklisted or not whitelisted
                    noPreview = True
                     
                
            else: #no filepath for file in xml is given 
                codeIndex, tag = self.__handleSourceCode("","")
                newStackElement = newStackElement.replace('*FILE*', 'no filename available')
                noPreview = True

            if noPreview:
                newStackElement = newStackElement.replace('*CODE_VAR*', "'None'")
                newStackElement = newStackElement.replace('*CODE_ID_VAR*',  "'None'")
                newStackElement = newStackElement.replace('*LINE_OF_CODE*', "'None'")
                insertPosition = newStackElement.find('btn"') 
                newStackElement = newStackElement[:insertPosition] + "grey " + newStackElement[insertPosition:] 

            
            self.__createSnippetEntry(frame, elementNumber, tag, codeIndex)
            callStack += newStackElement #append stack element
            elementNumber += 1
            self.__callStackNumber += 1 #increase global call stack number (used for reference variables)

        return callStack

    def __createErrorList(self):
        self.__strErrors = str()
        
        errorTemplate = self.__htmlTemplates.find('error_entry').text
        errorList = self.__reportRoot.findall('error')
        self.__numberOfErrors = len(errorList)
        
        for error in errorList:
            self.__errorNumber += 1
            outputID = "output_"+str(self.__errorNumber)+"_"
            newError = errorTemplate.replace('*ERROR_ID*', self.adjText(error.find('unique').text))
            newError = newError.replace('*ERROR_TYPE*', self.adjText(error.find('kind').text))
            
            xwhat = error.findall('xwhat')
            newError = newError.replace('*XWHAT_TEXT_1*', self.adjText(xwhat[0].find('text').text))
            newError = newError.replace('*XWHAT_TEXT_2*', self.adjText(xwhat[1].find('text').text))

            self.__errorHeading = str() #reset errorHeading, will be filled filled by __createCallStack
            newError = newError.replace('*CALL_STACK_ENTRIES_1*', self.__createCallStack(error, 0, outputID))
            newError = newError.replace('*CALL_STACK_ENTRIES_2*', self.__createCallStack(error, 1, outputID))
            newError = newError.replace('*OUTPUT_ID_1*', outputID+'0')
            newError = newError.replace('*OUTPUT_ID_2*', outputID+'1')
            newError = newError.replace('*ERROR_HEADING*', self.__errorHeading)

            self.__strErrors += newError

    def __createHeader(self):
        headerInformation = self.__getHeader()
        self.htmlReport = self.__htmlTemplates.find('base_entry').text
        self.htmlReport = self.htmlReport.replace('*DATE*', headerInformation[0])
        self.htmlReport = self.htmlReport.replace('*TIME*', headerInformation[1])
        self.htmlReport = self.htmlReport.replace('*DURATION*', headerInformation[2])
        self.htmlReport = self.htmlReport.replace('*ARGS*', headerInformation[3])
        self.htmlReport = self.htmlReport.replace('*EXE*', headerInformation[4])
        self.htmlReport = self.htmlReport.replace('*PROTOCOLVERSION*', headerInformation[5])
        self.htmlReport = self.htmlReport.replace('*PROTOCOLTOOL*', headerInformation[6])
        self.htmlReport = self.htmlReport.replace('*NUMBER_OF_ERRORS*', str(self.__numberOfErrors))
        self.htmlReport = self.htmlReport.replace('*ERROR_ENTRIES*', self.__strErrors)

    def __createReport(self):
        self.__createErrorList()
        self.__createHeader()
        self.__createCodeVars()

    def adjText(self, text): #change html symbols
        text = text.replace('&', '&amp;')
        text = text.replace('<', '&lt;')
        text = text.replace('>', '&gt;')
        text = text.replace('"', '&quot;')
        text = text.replace('\\', '/')
        return text




def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("-i", "--inputFile", help='input file', type=str)
    parser.add_argument("-o", "--outputDirectory", help='output directory', type=str)
    args = parser.parse_args()
    
    inFile = args.inputFile
    targetDirectory = args.outputDirectory

    if DEBUG:
        if inFile == None:
            inFile = 'test_files/test.xml'    
        if targetDirectory == None:
            targetDirectory = 'test_files/output'
        

    try:
        if not os.path.isfile(inFile):
            print("Your input file does not exist")
            return 0
    except TypeError:
        print("You must specify an input file")
        return 0
    

    report = ReportCreator(inFile)

    if report.succesfullReportCreation:

        if not os.path.isdir(targetDirectory):
            os.mkdir(targetDirectory)

        #write report to destination
        output = open(targetDirectory+'/index.html', mode='w')
        output.write(report.htmlReport)
        output.close()

        #copy needed files to destination
        if  os.path.isdir(targetDirectory+"/css"):
            shutil.rmtree(targetDirectory+"/css")
        if  os.path.isdir(targetDirectory+"/js"):
            shutil.rmtree(targetDirectory+"/js")
        shutil.copytree(g_CSSPATH, targetDirectory+"/css")
        shutil.copytree(g_JSPATH, targetDirectory+"/js")
        return 0

    else:
        return -1


if __name__ == "__main__":
    main()