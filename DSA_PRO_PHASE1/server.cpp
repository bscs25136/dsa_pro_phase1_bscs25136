// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)

#include "helper.h"
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <cerrno>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdint>
#include <cstdio>
#include <stack>
using namespace std;

// ---- Constants ----S


// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack {
    struct Node
    {
        T data;
        Node* next;
        Node(const T& val) {
            data = val;
            next = nullptr;
        }
    };
    Node* top;
    int32_t count;
public:
    Stack() {
        top = nullptr;
        count = 0;
    }
    void push(const T& val) {
        if (count < MAX_STACK_DEPTH) {
            Node* n = new Node(val);
            n->next = top;
            top = n;
            count++;
            return;
        }
    }

    T pop() {
        if (count == 0) {
            throw runtime_error("Stack is empty\n");
        }
        Node* temp = top;
        T val = temp->data;
        top = top->next;
        delete temp;
        count--;
        return val;
    }

    T& peek() {
        if (count == 0) {
            throw runtime_error("Stack is empty\n");
        }
        return top->data;
    }

    bool isEmpty() {
        return count == 0;
    }

    int32_t depth() {
        return count;
    }

    int32_t snapshot_into(T out[], int32_t maxLen) {
        Node* temp = top;
        int32_t i = 0;
        while (temp != nullptr && i < maxLen) {
            out[i] = temp->data;
            i++;
            temp = temp->next;
        }
        return i;
    }
};

// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode {
    Snapshot* data;
    TimelineNode* next;
    TimelineNode* prev;
    TimelineNode(Snapshot* s) {
        data = s;
        next = nullptr;
        prev = nullptr;
    }
};
class Timeline
{
    TimelineNode* head, * tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot* s) {
        if (stepCount == 0) {
            TimelineNode* t = new TimelineNode(s);
            head = tail = t;
            stepCount++;
            return;
        }
        TimelineNode* t = new TimelineNode(s);
        t->prev = tail;
        tail->next = t;
        tail = t;
        stepCount++;
        return;
    }
    TimelineNode* begin()
    {
        if (stepCount != 0) {
            return head;
        }
        else {
            throw runtime_error("No snapshots yet\n");
        }
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

// Core structs


bool writeHeader(FILE* f, const TTDBHeader& h)
{
    if (fwrite(h.magic, 1, 4, f) != 4) {
        return false;
    }
    if (fwrite(&h.version, sizeof(int32_t), 1, f) != 1) {
        return false;
    }
    if (fwrite(&h.stepCount, sizeof(int32_t), 1, f) != 1) {
        return false;
    }
    if (fwrite(&h.indexOffset, sizeof(int64_t), 1, f) != 1) {
        return false;
    }
    return true;
    // placeholder for other two data members
}

// resolve.bin - bookkeeping



// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream& in, string& out)
{
    while (getline(in, out)) {
        if (!out.empty()) {
            return true;
        }
    }
    return false;
}
string firstWord(const string& line)
{
    int i = 0;
    string s = "";
    while (i < line.length() && line[i] != ' ') {
        s += line[i];
        i++;
    }
    return s;
}
string secondWord(const string& line)
{
    int i = 0;
    string s = "";
    while (i < line.length() && line[i] != ' ') {
        i++;
    }
    while (i < line.length() && line[i] == ' ')
    {
        i++;
    }

    while (i < line.length() && line[i] != ' ') {
        s += line[i];
        i++;
    }
    return s;


}
bool validateProgram(const char* sourcePath)
{
    Stack<string> strStack;
    ifstream fin;
    string readLine;
    fin.open(sourcePath, ios::binary);
    if (!fin) {
        return false;
    }
    string firstW;
    while ((readSourceLine(fin, readLine))) {
        firstW = firstWord(readLine);
        if (firstW == "func") {
            if (strStack.isEmpty()) {
                strStack.push(firstW);
            }
            else {
                return false;
            }
        }
        else {
            if (firstW == "func_end") {
                if (strStack.isEmpty()) {
                    return false;
                }
                else {
                    strStack.pop();
                }
            }
        }
    }
    if (strStack.isEmpty()) {
        return true;
    }
    return false;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE* f, int64_t offsetField, const string& text)
{
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    int32_t sizeofString = text.size();
    fwrite(&sizeofString, sizeof(int32_t), 1, f);
    fwrite(text.data(), 1, text.size(), f);

    return offsetField;
}
int64_t readResolveRecord(FILE* f, string& outText)
{
    int64_t offsetByte;
    int32_t stringSize;

    fread(&offsetByte, sizeof(int64_t), 1, f);
    fread(&stringSize, sizeof(int32_t), 1, f);
    outText.resize(stringSize);
    fread(&outText[0], 1, stringSize, f);
    return offsetByte;
}
int64_t resolveProgram(const char* sourcePath, const char* resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    ifstream fin(sourcePath, ios::binary);
    if (!fin) {
        return -1;
    }
    FILE* resolveFile_fout = fopen(resolveBinPath, "wb+");
    if (!resolveFile_fout) {
        return -1;
    }
    int64_t offset = 0;
    int64_t mainOffset;
    string line;
    string firstW;
    string secondW;
    bool flag = false;
    while (readSourceLine(fin, line)) {
        writeResolveRecord(resolveFile_fout, offset, line);
        firstW = firstWord(line);
        secondW = secondWord(line);

        if (firstW == "func") {
            funcArray[funcCount].funcName = secondW;
            funcArray[funcCount].byteOffsetInResolveBin = offset;
            funcCount++;
            if (secondW == "main") {
                mainOffset = offset;
                flag = true;
            }
        }
        else if (firstW == "call") {
            patches[patchCount].targetFuncName = secondW;
            patches[patchCount].byteOffsetOfOffsetField = offset;
            patchCount++;
        }
        offset = offset + 8 + 4 + line.size();
    }
    if (flag == false) {
        fin.close();
        fclose(resolveFile_fout);
        return -1;
    }
    else {
        flag = false;
        int j = 0;
        for (int i = 0; i < patchCount; i++) {
            while (flag == false && j < funcCount) {
                if (patches[i].targetFuncName == funcArray[j].funcName) {
                    flag = true;
                    fseek(resolveFile_fout, patches[i].byteOffsetOfOffsetField, SEEK_SET);
                    fwrite(&funcArray[j].byteOffsetInResolveBin, sizeof(int64_t), 1, resolveFile_fout);

                }
                else {
                    j++;
                }
            }
            if (flag == false) {
                fin.close();
                fclose(resolveFile_fout);
                return -1;
            }
            else {
                j = 0;
                flag = false;
            }
        }
        fin.close();
        fclose(resolveFile_fout);
        return mainOffset;
    }
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string& line, Token tokens[], int32_t maxTokens)
{
    int i = 0;
    int32_t tokenNum = 0;
    string s = "";
    while (i < line.size() && line[i] == ' ') {
        i++;
    }
    while (i < line.size()) {
        if (line[i] != ' ') {
            s += line[i];
            i++;
        }
        else {
            i++;
            if (tokenNum < maxTokens) {
                if (tokenNum == 0) {
                    tokens[tokenNum].type = KEYWORD;
                }
                else if (tokenNum == 1) {
                    tokens[tokenNum].type = IDENTIFIER;

                }
                else {
                    tokens[tokenNum].type = PARAM;
                }
                tokens[tokenNum].text = s;
                tokenNum++;
                s = "";
            }
        }
    }
    if (tokenNum >= 2) {
        if (tokenNum < maxTokens) {
            tokens[tokenNum].type = PARAM;
            tokens[tokenNum].text = s;
            tokenNum++;
        }
    }
    else {
        if (tokenNum == 0) {
            tokens[tokenNum].type = KEYWORD;
        }
        else {
            tokens[tokenNum].type = IDENTIFIER;
        }
        tokens[tokenNum].text = s;
        tokenNum++;
    }

    return tokenNum;
}
Snapshot* buildSnapshot(Stack<Frame>& callStack)
{
    Snapshot* snap = new Snapshot();
    snap->stackDepth = callStack.depth();
    callStack.snapshot_into(snap->callStack, MAX_STACK_DEPTH);
    return snap;
}
Variable* findVariable(Frame& frame, const string& name) {
    for (int i = 0; i < frame.argc; i++) {
        if (frame.argv[i].name == name) {
            return &frame.argv[i];
        }
    }

    for (int i = 0; i < frame.localCount; i++) {
        if (frame.locals[i].name == name) {
            return &frame.locals[i];
        }
    }

    return nullptr;
}
struct argsUpdate {
    int32_t argC;
    string callerVar[MAX_VARS_PER_FRAME];
};
bool executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    bool succesfulExecution = true;
    Stack<Frame> callStack;
    Stack<argsUpdate> callerVarStack;
    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    mainFrame.returnLine = -1;
    callStack.push(mainFrame);
    FILE* resolveFile_fin = fopen(resolveBinPath, "rb");
    if (!resolveFile_fin) {
        cout << "Unable to open resolve file\n";
        return false;
    }
    fseek(resolveFile_fin, 0, SEEK_END);
    int64_t fileSize = ftell(resolveFile_fin);
    fseek(resolveFile_fin, mainOffset, SEEK_SET);
    string executionLine;
    int64_t offset;
    int64_t nextOffset;
    Token lineTokens[MAX_TOKENS];
    bool executed = false;
    while (ftell(resolveFile_fin) < fileSize) {
        offset = readResolveRecord(resolveFile_fin, executionLine);
        nextOffset = ftell(resolveFile_fin);
        int tokenCt = tokenizeLine(executionLine, lineTokens, MAX_TOKENS);

        if (lineTokens[0].text == "func") {
            Frame& topFrame = callStack.peek();
            int funcPara = tokenCt - 2;

            if (funcPara != topFrame.argc) {
                cout << "Argument count mismatch in function: " << topFrame.func_name << '\n';
               succesfulExecution = false;
                break;
            }

            for (int i = 0; i < topFrame.argc; i++) {
                topFrame.argv[i].name = lineTokens[i + 2].text;
            }
        }
        else if (lineTokens[0].text == "call") {
            Frame& callerFrame = callStack.peek();

            Frame stackFrame;
            stackFrame.localCount = 0;
            stackFrame.func_name = lineTokens[1].text;
            int i = 0;
            stackFrame.argc = tokenCt - 2;
            if (stackFrame.argc > MAX_VARS_PER_FRAME) {
                cout << "Too many function arguments\n";
                succesfulExecution = false;
                break;
            }

            if (callStack.depth() >= MAX_STACK_DEPTH) {
                cout << "Maximum call stack depth reached\n";
                succesfulExecution = false;
                break;
            }
            bool valid = true;

            argsUpdate info{};
            info.argC = stackFrame.argc;
            string arg;
            bool isNumber = true;
            bool foundFlag = false;

            for (int i = 0; i < stackFrame.argc; i++) {
                isNumber = true;
                arg = lineTokens[i + 2].text;
                for (int j = 0; j < arg.size(); j++) {
                    if (!isdigit(arg[j])) {
                        isNumber = false;
                        break;
                    }
                }
                if (isNumber) {
                    stackFrame.argv[i].value = stoi(lineTokens[i + 2].text);
                    info.callerVar[i] = "";

                }
                else {
                    int val;
                    foundFlag = false;
                    for (int j = 0; j < callerFrame.argc; j++) {
                        if (callerFrame.argv[j].name == arg) {
                            val = callerFrame.argv[j].value;
                            foundFlag = true;
                            break;

                        }
                    }

                    if (foundFlag == false) {
                        for (int j = 0; j < callerFrame.localCount; j++) {
                            if (callerFrame.locals[j].name == arg) {
                                val = callerFrame.locals[j].value;
                                foundFlag = true;
                                break;
                            }
                        }
                    }
                    if (foundFlag == false) {
                        cout << "Undefined argument variable: " << arg << '\n';
                        
                        valid = false;
                        break;
                    }

                    stackFrame.argv[i].value = val;
                    info.callerVar[i] = arg;
                }
            }
            if (valid) {
                stackFrame.returnLine = nextOffset;
                fseek(resolveFile_fin, offset, SEEK_SET);

                callerVarStack.push(info);
                callStack.push(stackFrame);

            }
            else {
                succesfulExecution = false;
                break;
            }
        }
        else if (lineTokens[0].text == "add") {
            bool flag = false;
            Frame& topFrame = callStack.peek();
            bool isNumber = true;
            for (int i = 0; i < lineTokens[2].text.size(); i++) {
                if (!isdigit(lineTokens[2].text[i])) {
                    isNumber = false;
                    break;
                }
            }
            if (isNumber) {
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[1].text) {
                        topFrame.argv[i].value += stoi(lineTokens[2].text);
                        flag = true;
                        break;
                    }
                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[1].text) {
                            topFrame.locals[i].value += stoi(lineTokens[2].text);
                            flag = true;
                            break;
                        }
                    }
                }
                if (flag == false) {
                    cout << "Destination variable not found\n";
                    succesfulExecution = false;
                    break;
                }
            }
            else {
                int val;
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[2].text) {
                        val = topFrame.argv[i].value;
                        flag = true;
                        break;
                    }
                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[2].text) {
                            val = topFrame.locals[i].value;
                            flag = true;
                            break;
                        }
                    }
                }
                if (flag == false) {
                    cout << "Other variable not found\n";
                    succesfulExecution = false;
                    break;
                }
                flag = false;
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[1].text) {
                        topFrame.argv[i].value += val;
                        flag = true;
                        break;
                    }

                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[1].text) {
                            topFrame.locals[i].value += val;
                            flag = true;
                            break;
                        }

                    }
                }
                if (flag == false) {
                    cout << "Destination variable not found\n";
                    succesfulExecution = false;
                    break;
                }
            }
        }

        else if (lineTokens[0].text == "mul") {
            bool flag = false;
            Frame& topFrame = callStack.peek();
            bool isNumber = true;
            for (int i = 0; i < lineTokens[2].text.size(); i++) {
                if (!isdigit(lineTokens[2].text[i])) {
                    isNumber = false;
                    break;
                }
            }
            if (isNumber) {
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[1].text) {
                        topFrame.argv[i].value *= stoi(lineTokens[2].text);
                        flag = true;
                        break;
                    }
                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[1].text) {
                            topFrame.locals[i].value *= stoi(lineTokens[2].text);
                            flag = true;
                            break;
                        }
                    }
                }
                if (flag == false) {
                    cout << "Destination variable not found\n";
                    succesfulExecution = false;
                    break;
                }
            }
            else {
                int val;
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[2].text) {
                        val = topFrame.argv[i].value;
                        flag = true;
                        break;
                    }
                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[2].text) {
                            val = topFrame.locals[i].value;
                            flag = true;
                            break;
                        }
                    }
                }
                if (flag == false) {
                    cout << "Other variable not found\n";
                    succesfulExecution = false;
                    break;
                }
                flag = false;
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[1].text) {
                        topFrame.argv[i].value *= val;
                        flag = true;
                        break;
                    }

                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[1].text) {
                            topFrame.locals[i].value *= val;
                            flag = true;
                            break;
                        }

                    }
                }
                if (flag == false) {
                    cout << "Destination variable not found\n";
                    succesfulExecution = false;
                    break;
                }
            }
        }
        else if (lineTokens[0].text == "div") {
            bool flag = false;
            Frame& topFrame = callStack.peek();
            bool isNumber = true;
            for (int i = 0; i < lineTokens[2].text.size(); i++) {
                if (!isdigit(lineTokens[2].text[i])) {
                    isNumber = false;
                    break;
                }
            }
            if (isNumber) {
                if (stoi(lineTokens[2].text) == 0) {
                    cout << "Division by zero\n";
                    succesfulExecution = false;
                    break;
                }
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[1].text) {
                        topFrame.argv[i].value /= stoi(lineTokens[2].text);
                        flag = true;
                        break;
                    }
                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[1].text) {
                            topFrame.locals[i].value /= stoi(lineTokens[2].text);
                            flag = true;
                            break;
                        }
                    }
                }
                if (flag == false) {
                    cout << "Destination variable not found\n";
                    succesfulExecution = false;
                    break;
                }
            }
            else {
                int val;
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[2].text) {
                        val = topFrame.argv[i].value;
                        flag = true;
                        break;
                    }
                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[2].text) {
                            val = topFrame.locals[i].value;
                            flag = true;
                            break;
                        }
                    }
                }
                if (flag == false) {
                    cout << "Other variable not found\n";
                    succesfulExecution = false;
                    break;
                }
                if (val == 0) {
                    cout << "Division by zero\n";
                    succesfulExecution = false;
                    break;
                }
                flag = false;
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[1].text) {
                        topFrame.argv[i].value /= val;
                        flag = true;
                        break;
                    }

                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[1].text) {
                            topFrame.locals[i].value /= val;
                            flag = true;
                            break;
                        }

                    }
                }
                if (flag == false) {
                    cout << "Destination variable not found\n";
                    succesfulExecution = false;
                    break;
                }
            }
        }
        else if (lineTokens[0].text == "sub") {
            bool flag = false;
            Frame& topFrame = callStack.peek();
            bool isNumber = true;
            for (int i = 0; i < lineTokens[2].text.size(); i++) {
                if (!isdigit(lineTokens[2].text[i])) {
                    isNumber = false;
                    break;
                }
            }
            if (isNumber) {
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[1].text) {
                        topFrame.argv[i].value -= stoi(lineTokens[2].text);
                        flag = true;
                        break;
                    }
                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[1].text) {
                            topFrame.locals[i].value -= stoi(lineTokens[2].text);
                            flag = true;
                            break;
                        }
                    }
                }
                if (flag == false) {
                    cout << "Destination variable not found\n";
                    succesfulExecution = false;
                    break;
                }
            }
            else {
                int val;
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[2].text) {
                        val = topFrame.argv[i].value;
                        flag = true;
                        break;
                    }
                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[2].text) {
                            val = topFrame.locals[i].value;
                            flag = true;
                            break;
                        }
                    }
                }
                if (flag == false) {
                    cout << "Other variable not found\n";
                    succesfulExecution = false;
                    break;
                }
                flag = false;
                for (int i = 0; i < topFrame.argc; i++) {
                    if (topFrame.argv[i].name == lineTokens[1].text) {
                        topFrame.argv[i].value -= val;
                        flag = true;
                        break;
                    }

                }
                if (flag == false) {
                    for (int i = 0; i < topFrame.localCount; i++) {
                        if (topFrame.locals[i].name == lineTokens[1].text) {
                            topFrame.locals[i].value -= val;
                            flag = true;
                            break;
                        }

                    }
                }
                if (flag == false) {
                    cout << "Destination variable not found\n";
                    succesfulExecution = false;
                    break;
                }
            }
        }
        else if (lineTokens[0].text == "set") {
            bool flag = false;
            Frame& topFrame = callStack.peek();
            for (int i = 0; i < topFrame.argc; i++) {
                if (topFrame.argv[i].name == lineTokens[1].text) {
                    topFrame.argv[i].value = stoi(lineTokens[2].text);
                    flag = true;
                    break;
                }
            }
            if (flag == false) {
                for (int i = 0; i < topFrame.localCount; i++) {
                    if (topFrame.locals[i].name == lineTokens[1].text) {
                        topFrame.locals[i].value = stoi(lineTokens[2].text);
                        flag = true;
                        break;
                    }
                }
            }
            if (flag == false) {
                if (topFrame.localCount < MAX_VARS_PER_FRAME) {
                    topFrame.locals[topFrame.localCount].name = lineTokens[1].text;
                    topFrame.locals[topFrame.localCount].value = stoi(lineTokens[2].text);
                    topFrame.localCount++;
                    flag = true;
                }
                else {
                    cout << "Maximum local variables reached\n";
                    succesfulExecution = false;
                    break;
                }
            }
        }
        else if (lineTokens[0].text == "func_end") {
            Frame topFrame = callStack.peek();
            if (topFrame.func_name != "main") {
                argsUpdate info = callerVarStack.peek();
                int64_t returnOffset = topFrame.returnLine;
                callStack.pop();
                callerVarStack.pop();
                Frame& callerFrame = callStack.peek();
                for (int i = 0; i < info.argC; i++) {
                    if (info.callerVar[i].empty()) {
                        continue;
                    }
                    Variable* argFound = findVariable(callerFrame, info.callerVar[i]);
                    if (argFound != nullptr) {
                        argFound->value = topFrame.argv[i].value;
                    }
                }
                fseek(resolveFile_fin, returnOffset, SEEK_SET);

            }
            else {
                executed = true;
            }

        }
        Snapshot* snap = buildSnapshot(callStack);
        timeline.record(snap);
        if (executed == true) {
            break;
        }
    }
    fclose(resolveFile_fin);
    return succesfulExecution && executed;
}

bool serializeString(FILE* fileP, const string& s) {
    int32_t stringSize = s.size();
    if (fwrite(&stringSize, sizeof(int32_t), 1, fileP) != 1) {
        return false;
    }
    if (stringSize > 0) {
        if (fwrite(s.data(), 1, stringSize, fileP) != stringSize) {
            return false;
        }
    }
    return true;
}
bool serializeFrame(FILE* fileP, const Frame& frame) {
    if (!serializeString(fileP, frame.func_name)) {
        return false;
    }
    if (fwrite(&frame.argc, sizeof(int32_t), 1, fileP) != 1) {
        return false;
    }
    for (int i = 0; i < frame.argc; i++) {
        if (!serializeString(fileP, frame.argv[i].name)) {
            return false;
        }
        if (fwrite(&frame.argv[i].value, sizeof(int32_t), 1, fileP) != 1) {
            return false;
        }
    }
    if (fwrite(&frame.returnLine, sizeof(int64_t), 1, fileP) != 1) {
        return false;

    }
    if(fwrite(&frame.localCount, sizeof(int32_t), 1, fileP)!=1) {
        return false;

    }
    for (int i = 0; i < frame.localCount; i++) {
        if (!serializeString(fileP, frame.locals[i].name)) {
            return false;
        }
        if (fwrite(&frame.locals[i].value, sizeof(int32_t), 1, fileP) != 1) {
            return false;
        }
    }
    return true;
}
bool writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    FILE* fileTdbg = fopen(tdbgPath, "wb+");
    if (!fileTdbg) {
        cout << "Unable to open\n";
        return false;
    }
    TTDBHeader header;
    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';
    header.version = 1;
    header.indexOffset = 0;
    header.stepCount = timeline.getStepCount();
    if (!writeHeader(fileTdbg, header)) {
        cout << "Failed to write header\n";
        fclose(fileTdbg);
        return false;
    }
    if (header.stepCount <= 0) {
        fclose(fileTdbg);
        return false;
    }
    int64_t snapOffset;
    int64_t* snapshotPosition = new int64_t[header.stepCount];

    TimelineNode* snap = timeline.begin();
    int snapCount = 0;
    while (snap != nullptr) {
        snapOffset = ftell(fileTdbg);
        if (snapOffset == -1) {
            cout << "Failed to get snapshot offset\n";
            delete[] snapshotPosition;
            fclose(fileTdbg);
            return false;
        }
        if (snapCount >= header.stepCount) {
            cout << "Too many snapshots\n";
            delete[] snapshotPosition;
            fclose(fileTdbg);
            return false;
        }
        snapshotPosition[snapCount++] = snapOffset;
        if (fwrite(&snap->data->stackDepth, sizeof(int32_t), 1, fileTdbg) != 1) {
            cout << "Failed to write stack depth\n";
            delete[] snapshotPosition;
            fclose(fileTdbg);
            return false;
        }
        for (int i = 0; i < snap->data->stackDepth; i++) {
            if (!serializeFrame(fileTdbg, snap->data->callStack[i])) {
                cout << "Failed to serialize frame\n";
                delete[] snapshotPosition;
                fclose(fileTdbg);
                return false;
            }
        }
        snap = snap->next;
    }
    header.indexOffset = ftell(fileTdbg);
    if (header.indexOffset == -1) {
        cout << "Failed to get header offset\n";
        delete[] snapshotPosition;
        fclose(fileTdbg);
        return false;
    }
    if (snapCount != header.stepCount) {
        cout << "Snapshot count mismatch\n";
        delete[] snapshotPosition;
        fclose(fileTdbg);
        return false;
    }
    if (fwrite(snapshotPosition, sizeof(int64_t), snapCount, fileTdbg) != snapCount) {
        cout << "Failed to write dense index\n";
        delete[] snapshotPosition;
        fclose(fileTdbg);
        return false;
    }
    if (fseek(fileTdbg, 0, SEEK_SET) != 0) {
        cout << "Failed to move to beginning of file\n";
        delete[] snapshotPosition;
        fclose(fileTdbg);
        return false;
    }
    if (!writeHeader(fileTdbg, header)) {
        cout << "Failed to write header\n";
        delete[] snapshotPosition;
        fclose(fileTdbg);
        return false;
    }

    delete[] snapshotPosition;
    if (fclose(fileTdbg) != 0) {
        cout << "Failed to close TDBG file properly\n";
        return false;
    }

    return true;
}

int listeningSocket(int port) {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket == -1) {
        perror("Socket creation failed");
        return -1;
    }
    int opt = 1;

    if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR,
        &opt, sizeof(opt)) == -1) {
        perror("setsockopt failed");
        close(serverSocket);
        return -1;
    }
    sockaddr_in serverAddress{};

    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY;
    serverAddress.sin_port = htons(port);
    if (bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) == -1) {
        perror("failed bind\n");
        close(serverSocket);
        return -1;
    }
    if (listen(serverSocket, 10) == -1) {
        perror("falied listening");
        close(serverSocket);
        return -1;
    }

    cout << "Server listening on port " << port << endl;

    return serverSocket;
}

int acceptClient(int Socketserver){
    
    sockaddr_in clientAddress{};

    socklen_t clientAddressLength = sizeof(clientAddress);

    
    int clientSocket = accept(Socketserver,(struct sockaddr*)&clientAddress,&clientAddressLength);

    
    if (clientSocket == -1) {
        perror("Accept failed");
        return -1;
    }

   
    cout << "Client connected from "<< inet_ntoa(clientAddress.sin_addr)<< ":" << ntohs(clientAddress.sin_port)<< endl;

    return clientSocket;
}
bool receiveall_Bytes(int sock, char* buffer, size_t length){
    size_t recievedT = 0;

    while (recievedT < length){
        int bytesReceived = recv(sock,buffer + recievedT,length - recievedT,0);

        if (bytesReceived == 0){
            return false;
        }

        if (bytesReceived == -1){
            if (errno == EINTR){
                continue;
            }

            perror("Didnt recieve. Failed to recieve");
            return false;
        }

        recievedT = recievedT + bytesReceived;
    }

    return true;
}

bool sendAll_Bytes(int sock, const char* buffer, size_t length){
    size_t SentT = 0;

    while (SentT < length){
        int bytesSent = send(sock,buffer + SentT,length - SentT,MSG_NOSIGNAL);

        if (bytesSent == 0){
            return false;
        }

        if (bytesSent == -1){
            if (errno == EINTR){
                continue;
            }

            perror("Couldnt send.Send failed");
            return false;
        }

        SentT = SentT  + bytesSent;
    }

    return true;
}

bool receiveUpload(int clientSocket, const char* sourceFile){
    
    FILE* SourceFile = fopen(sourceFile, "wb");

    if (SourceFile == nullptr){
        perror("Could not open source file");
        return false;
    }

    TrailerChecker T_checker;

    char buffer[IO_BUFFER_SIZE];

 
    bool statusReceived = false;
    char status = -1;


    uint64_t totalBytes_Source = 0;

    vector<char> actualBytes;

  
    while (true){
        int bytesReceived = recv(clientSocket,buffer,sizeof(buffer),0);

        if (bytesReceived == 0){
            break;
        }

        if (bytesReceived == -1){
            if (errno == EINTR){
                continue;
            }

            perror("Upload receive failed");
            fclose(SourceFile);
            remove(sourceFile);
            return false;
        }

        T_checker.feed(buffer, bytesReceived, actualBytes);

        
        for (size_t i = 0; i < actualBytes.size(); i++) {
            char byte = actualBytes[i];
            if (!statusReceived){
                status = byte;
                statusReceived = true;

                if (status != 0){
                    cout << "Invalid upload status" << endl;
                    fclose(SourceFile);
                    remove(sourceFile);
                    return false;
                }

                continue;
            }

            totalBytes_Source++;

            if (totalBytes_Source > MAX_SOURCE_BYTES){
                cout << "Source file exceeds maximum size" << endl;
                fclose(SourceFile);
                remove(sourceFile);
                return false;
            }

            if (fwrite(&byte, 1, 1, SourceFile) != 1){
                perror("File write failed");
                fclose(SourceFile);
                remove(sourceFile);
                return false;
            }
        }
    }

    if (!statusReceived){
        cout << "Empty upload message" << endl;
        fclose(SourceFile);
        remove(sourceFile);
        return false;
    }


    if (!T_checker.verify()){
        cout << "Upload checksum mismatch" << endl;
        fclose(SourceFile);
        remove(sourceFile);
        return false;
    }
    if (totalBytes_Source == 0) {
        cout << "Received source file is empty" << endl;
        fclose(SourceFile);
        remove(sourceFile);
        return false;
    }
    if (fclose(SourceFile) != 0) {
        perror("Failed to close source file");
        remove(sourceFile);
        return false;
    }

    cout << "Source file received successfully" << endl;
    return true;
}
bool sendResponse(int clientSocket, uint8_t status,const char* filePath,const string& errorMsg){
    CRC32 Cal_checksum;

    FILE* tdbg_file = nullptr;

    if (status == 0) {

        tdbg_file = fopen(filePath, "rb");

        if (tdbg_file == nullptr) {
            perror("Could not open TDBG file");
            return false;
        }
    }

    char statusByte = static_cast<char>(status);

    if (!sendAll_Bytes(clientSocket, &statusByte, 1)) {
        if (tdbg_file != nullptr) {
            fclose(tdbg_file);
        }
        return false;
    }

    Cal_checksum.update(&statusByte, 1);

    if (status == 0){

        char buffer[IO_BUFFER_SIZE];

        while (true){
            size_t bytesRead = fread(buffer, 1, sizeof(buffer), tdbg_file);

            if (bytesRead > 0){
                
                Cal_checksum.update(buffer, bytesRead);

         
                if (!sendAll_Bytes(clientSocket, buffer, bytesRead)){
                    fclose(tdbg_file);
                    return false;
                }
            }

            if (bytesRead < sizeof(buffer)){
                if (feof(tdbg_file)){
                    break;
                }

                if (ferror(tdbg_file)){
                    perror("Error in reading TDBG file");
                    fclose(tdbg_file);
                    return false;
                }
            }
        }

        fclose(tdbg_file);
    }
    else{
        if (!errorMsg.empty()){
            Cal_checksum.update(errorMsg.data(), errorMsg.size());

            if (!sendAll_Bytes(clientSocket,
                errorMsg.data(),
                errorMsg.size())){
                return false;
            }
        }
    }
    uint32_t crcValue = Cal_checksum.value();

    char checksumBytes[4];

    checksumBytes[0] = static_cast<char>((crcValue >> 24) & 0xFF);
    checksumBytes[1] = static_cast<char>((crcValue >> 16) & 0xFF);
    checksumBytes[2] = static_cast<char>((crcValue >> 8) & 0xFF);
    checksumBytes[3] = static_cast<char>(crcValue & 0xFF);

    if (!sendAll_Bytes(clientSocket, checksumBytes, 4)){
        return false;
    }

    return true;
}

bool clientRequest(int clientSocket) {
    if (!receiveUpload(clientSocket, "source.bin")) {
        sendResponse(clientSocket, 1, nullptr,"Failed to receive source file");
        return false;
    }
    if (!validateProgram("source.bin")) {
        sendResponse(clientSocket, 1, nullptr, "Program didnt pass validation");
        return false;
    }
    int64_t mainOffset =resolveProgram("source.bin", "resolve.bin");

    if (mainOffset == -1) {
        sendResponse(clientSocket, 1, nullptr, "Program failed resolution");
        return false;
    }
    Timeline prog_Timeline;
    if (!executeProgram("resolve.bin", mainOffset, prog_Timeline)) {
        sendResponse(clientSocket, 1, nullptr, "Failed execution");
        return false;
    }
    if (!writeTdbg(prog_Timeline, "session.tdbg")){
        sendResponse(clientSocket, 1, nullptr,"Failed to generate TDBG file");
        return false;
    }
    if (!sendResponse(clientSocket, 0, "session.tdbg", "")){
        cout << "Failed to send TDBG response\n";
        return false;
    }

    return true;
}
// main section
int32_t main(int argc, char* argv[])
{
    int port = 5001;
    int serverSide_Socket = listeningSocket(port);
    if (serverSide_Socket == -1) {
        cout << "Failed to start server\n";
        return 1;
    }

    cout << "Server socket is listening and using the port " << port << endl;

    while (true) {
        int clientSide_Socket = acceptClient(serverSide_Socket);
        if (clientSide_Socket == -1) {
            continue;
        }
        cout << "Load Balancer connected\n";

        clientRequest(clientSide_Socket);

        shutdown(clientSide_Socket, SHUT_RDWR);
        close(clientSide_Socket);

        cout << "Client side connection closed\n";
    }
    close(serverSide_Socket);

    return 0;
}
   