// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
#include <stack>
using namespace std;

// ---- Constants ----S
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

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
            return;
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
struct TimelineNode{
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
    void record(Snapshot* s){
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
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE* f, const TTDBHeader& h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);

    // placeholder for other two data members
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



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
                return -1;
            }
            else {
                j = 0;
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
    if (tokenNum < maxTokens) {
        tokens[tokenNum].type = PARAM;
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
void executeProgram(const char* resolveBinPath, int64_t mainOffset, Timeline& timeline)
{
    Stack<Frame> callStack;
    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.localCount = 0;
    mainFrame.returnLine = -1;
    callStack.push(mainFrame);
    FILE* resolveFile_fin = fopen(resolveBinPath, "rb");
    if (!resolveFile_fin) {
        cout << "Unable to open resolve file\n";
        return;
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
            for (int i = 0; i < topFrame.argc; i++) {
                topFrame.argv[i].name = lineTokens[i + 2].text;
            }
        }
        else if (lineTokens[0].text == "call") {
            Frame stackFrame;
            stackFrame.localCount = 0;
            stackFrame.func_name = lineTokens[1].text;
            int i = 0;
            stackFrame.argc = tokenCt - 2;
            for (int i = 0; i < stackFrame.argc; i++) {
                stackFrame.argv[i].value = stoi(lineTokens[i + 2].text);
            }
            stackFrame.returnLine = nextOffset;
            fseek(resolveFile_fin, offset, SEEK_SET);
            callStack.push(stackFrame);
        }
        else if (lineTokens[0].text == "add") {
            bool flag = false;
            Frame& topFrame = callStack.peek();
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
                        break;
                    }
                }
            }
        }
        else if (lineTokens[0].text == "mul") {
            bool flag = false;
            Frame& topFrame = callStack.peek();
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
                        break;
                    }
                }
            }
        }
        else if (lineTokens[0].text == "div") {
            bool flag = false;
            Frame& topFrame = callStack.peek();
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
                        break;
                    }
                }
            }
        }
        else if (lineTokens[0].text == "sub") {
            bool flag = false;
            Frame& topFrame = callStack.peek();
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
                        break;
                    }
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

            }
        }
        else if (lineTokens[0].text == "func_end") {

            Frame topFrame = callStack.peek();
            if (topFrame.func_name != "main") {

                int64_t returnOffset = topFrame.returnLine;
                callStack.pop();
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
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline& timeline, const char* tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}