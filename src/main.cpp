// =====================================================
// ADVANCED SEARCH ENGINE - Ranking & Autocomplete
//
// Phases 1-9:
//   1  Preprocessing / tokenization     6  Auto-loading, top-K, timing
//   2  Inverted index                   7  BM25, autocorrect, field search,
//   3  Trie autocomplete                   query suggestions, comparison
//   4  TF-IDF + cosine similarity       8  Persistent history, combined
//   5  Boolean / phrase / highlight        autocomplete, optimizations
//                                       9  Diagnostics, benchmark, self-tests
//
// Usage:
//   search_engine.exe                        (default folders)
//   search_engine.exe <dataFolder> [historyFile]
//   search_engine.exe --selftest             (automated tests, exits)
// =====================================================

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <cctype>
#include <chrono>
#include <limits>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

#include "Trie.h"

using namespace std;

typedef vector<pair<int, double>> RankedList;

// =====================================================
// CONFIGURATION
// =====================================================

#ifdef _WIN32
const char* DEFAULT_DATA_FOLDER = "E:/seo/Data";
#else
const char* DEFAULT_DATA_FOLDER = "Data";
#endif

const char* DEFAULT_HISTORY_FILE = "search_history.txt";

const double BM25_K1 = 1.5;
const double BM25_B = 0.75;
const double TITLE_BOOST = 2.0;      // Title matches count double in "both" mode
const size_t CACHE_LIMIT = 200;      // Max cached result lists

// =====================================================
// GLOBAL DATA
// =====================================================

enum Field { FIELD_TITLE, FIELD_CONTENT, FIELD_BOTH };

vector<string> documents;
vector<string> documentNames;
vector<string> documentTitles;       // Title shown to the user
vector<string> titleSearchText;      // Text indexed for the title field

// Phase 8: tokenized / normalized forms computed once at build time
vector<vector<string>> docTokens;
vector<vector<string>> titleTokens;
vector<string> normalizedDocuments;

unordered_map<string, unordered_map<int, int>> invertedIndex;
unordered_map<string, int> documentFrequency;
unordered_map<string, int> corpusTermFrequency;

unordered_map<string, unordered_map<int, int>> titleIndex;
vector<int> contentLengths;
vector<int> titleLengths;
double avgContentLength = 0.0;
double avgTitleLength = 0.0;

vector<unordered_map<string, double>> tfidfVectors;
vector<double> documentNorms;

Trie autocompleteTrie;

// Search history (Phase 6) + frequency log (Phase 7) + persistence (Phase 8)
vector<string> searchHistory;
unordered_map<string, int> queryCounts;
string historyFile = DEFAULT_HISTORY_FILE;
bool historyEnabled = false;         // Only write to disk when enabled
bool historyWriteWarned = false;

// Phase 8: result cache
unordered_map<string, RankedList> resultCache;
long cacheHits = 0;
long cacheMisses = 0;
bool cacheEnabled = true;

// Phase 9: diagnostics
double indexBuildMs = 0.0;
string dataFolder = DEFAULT_DATA_FOLDER;

unordered_set<string> stopWords = {
    "a", "an", "the", "is", "are", "was", "were",
    "in", "on", "at", "to", "for", "of", "and",
    "or", "but", "with", "this", "that", "it",
    "as", "by", "from", "be", "been", "being",
    "has", "have", "had", "do", "does", "did"
};


// =====================================================
// PHASE 1: TEXT PREPROCESSING
// =====================================================

string preprocess(const string& text) {

    string result;

    for (unsigned char ch : text) {

        if (isalnum(ch)) {
            result += static_cast<char>(tolower(ch));
        }
        else {
            result += ' ';
        }
    }

    return result;
}


// =====================================================
// TOKENIZATION
// =====================================================

vector<string> tokenize(const string& text) {

    string cleanedText = preprocess(text);

    stringstream ss(cleanedText);

    vector<string> words;
    string word;

    while (ss >> word) {

        if (stopWords.find(word) == stopWords.end()) {
            words.push_back(word);
        }
    }

    return words;
}


// =====================================================
// STRING HELPERS
// =====================================================

string trimString(const string& text) {

    size_t start = 0;
    size_t end = text.size();

    while (start < end &&
           isspace(static_cast<unsigned char>(text[start]))) {
        start++;
    }

    while (end > start &&
           isspace(static_cast<unsigned char>(text[end - 1]))) {
        end--;
    }

    return text.substr(start, end - start);
}

string toLowerString(string text) {

    transform(
        text.begin(), text.end(), text.begin(),
        [](unsigned char c) {
            return static_cast<char>(tolower(c));
        }
    );

    return text;
}

// File name without folder and extension: "E:/seo/Data/doc1.txt" -> "doc1"
string fileStem(const string& path) {

    size_t slash = path.find_last_of("/\\");

    string name =
        (slash == string::npos) ? path : path.substr(slash + 1);

    size_t dot = name.find_last_of('.');

    if (dot != string::npos && dot > 0) {
        name = name.substr(0, dot);
    }

    return name;
}

// Lowercase, trim, and collapse repeated spaces.
string normalizeQuery(const string& query) {

    string result;
    bool lastWasSpace = true;

    for (unsigned char ch : query) {

        if (isspace(ch)) {

            if (!lastWasSpace) {
                result += ' ';
                lastWasSpace = true;
            }
        }
        else {
            result += static_cast<char>(tolower(ch));
            lastWasSpace = false;
        }
    }

    if (!result.empty() && result[result.size() - 1] == ' ') {
        result.erase(result.size() - 1);
    }

    return result;
}

string fieldName(Field field) {

    if (field == FIELD_TITLE) return "Title";
    if (field == FIELD_CONTENT) return "Content";
    return "Title + Content";
}

string intToString(long value) {

    ostringstream out;
    out << value;
    return out.str();
}

// Temporarily redirects cout (used by the self-tests to keep output clean).
struct OutputSilencer {

    ostringstream sink;
    streambuf* previous;

    OutputSilencer() : previous(cout.rdbuf(sink.rdbuf())) {}

    ~OutputSilencer() {
        cout.rdbuf(previous);
    }
};


// =====================================================
// DOCUMENT READING
// =====================================================

// Reads the whole file. firstLine receives the first non-empty line,
// which is used as the document title.
string readDocument(const string& filePath, string& firstLine) {

    firstLine = "";

    ifstream file(filePath.c_str());

    if (!file.is_open()) {

        cerr << "Unable to open: "
             << filePath << endl;

        return "";
    }

    string content;
    string line;

    while (getline(file, line)) {

        if (firstLine.empty()) {
            firstLine = trimString(line);
        }

        content += line + " ";
    }

    file.close();

    return content;
}


// =====================================================
// ADD DOCUMENT
// =====================================================

// Registers a document from text already in memory.
void addDocumentText(
    const string& name,
    const string& firstLine,
    const string& content
) {

    string stem = fileStem(name);

    // Title: first line if it is short enough, otherwise the file name.
    string title = firstLine;

    if (title.empty() || title.size() > 80) {
        title = stem;
    }

    documents.push_back(content);
    documentNames.push_back(name);
    documentTitles.push_back(title);

    // Title field text includes the file name so "doc2" is searchable too.
    titleSearchText.push_back(
        (title == stem) ? stem : title + " " + stem
    );
}

void addDocument(const string& filePath) {

    string firstLine;
    string content = readDocument(filePath, firstLine);

    if (content.empty()) {

        cout << "Skipping unavailable file: "
             << filePath << endl;

        return;
    }

    addDocumentText(filePath, firstLine, content);

    cout << "Loaded: " << filePath << endl;
}


// =====================================================
// PHASE 6.1: AUTOMATIC TXT DOCUMENT LOADING
// (Windows API on Windows; POSIX fallback elsewhere)
// =====================================================

vector<string> listTextFiles(const string& folderPath) {

    vector<string> filePaths;

#ifdef _WIN32

    string searchPattern = folderPath + "\\*.txt";

    WIN32_FIND_DATAA findData;

    HANDLE hFind = FindFirstFileA(
        searchPattern.c_str(),
        &findData
    );

    if (hFind == INVALID_HANDLE_VALUE) {
        return filePaths;
    }

    do {
        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            filePaths.push_back(
                folderPath + "/" + findData.cFileName
            );
        }

    } while (FindNextFileA(hFind, &findData));

    FindClose(hFind);

#else

    DIR* dir = opendir(folderPath.c_str());

    if (dir == nullptr) {
        return filePaths;
    }

    struct dirent* entry;

    while ((entry = readdir(dir)) != nullptr) {

        string name = entry->d_name;

        if (name.size() > 4 &&
            toLowerString(name.substr(name.size() - 4)) == ".txt") {

            filePaths.push_back(folderPath + "/" + name);
        }
    }

    closedir(dir);

#endif

    // Keep document IDs consistent between runs.
    sort(filePaths.begin(), filePaths.end());

    return filePaths;
}

void loadAllDocuments(const string& folderPath) {

    vector<string> filePaths = listTextFiles(folderPath);

    if (filePaths.empty()) {
        cout << "No .txt files found in: "
             << folderPath << endl;
        return;
    }

    for (const string& filePath : filePaths) {
        addDocument(filePath);
    }

    cout << "\nTotal documents loaded: " << documents.size() << endl;
}


// =====================================================
// PHASE 2: BUILD INVERTED INDEX
// =====================================================

void buildInvertedIndex() {

    invertedIndex.clear();
    documentFrequency.clear();
    corpusTermFrequency.clear();

    for (int docId = 0;
         docId < static_cast<int>(documents.size());
         docId++) {

        unordered_map<string, int> termCount;

        for (const string& word : docTokens[docId]) {

            termCount[word]++;
            corpusTermFrequency[word]++;
        }

        for (const auto& entry : termCount) {

            invertedIndex[entry.first][docId] = entry.second;

            documentFrequency[entry.first]++;
        }
    }

    // Build Trie vocabulary
    for (const auto& entry : corpusTermFrequency) {

        autocompleteTrie.insert(
            entry.first,
            entry.second
        );
    }

    cout << "\nInverted Index built successfully!\n";
}


// =====================================================
// DISPLAY INVERTED INDEX
// =====================================================

void displayInvertedIndex() {

    cout << "\n========== INVERTED INDEX ==========\n";

    vector<string> words;

    for (const auto& entry : invertedIndex) {
        words.push_back(entry.first);
    }

    sort(words.begin(), words.end());

    for (const string& word : words) {

        cout << "\n" << word << " -> ";

        for (const auto& posting : invertedIndex[word]) {

            cout << "[Doc "
                 << posting.first + 1
                 << ": TF "
                 << posting.second
                 << "] ";
        }

        cout << "| DF: "
             << documentFrequency[word];
    }

    cout << "\n";
}


// =====================================================
// PHASE 4: BUILD TF-IDF VECTORS
// =====================================================

void buildTFIDF() {

    int totalDocuments = documents.size();

    tfidfVectors.clear();
    documentNorms.clear();

    tfidfVectors.resize(totalDocuments);
    documentNorms.resize(totalDocuments, 0.0);

    for (int docId = 0;
         docId < totalDocuments;
         docId++) {

        const vector<string>& words = docTokens[docId];

        if (words.empty()) {
            continue;
        }

        unordered_map<string, int> termFrequency;

        for (const string& word : words) {
            termFrequency[word]++;
        }

        double totalWords = words.size();

        for (const auto& entry : termFrequency) {

            double tf = entry.second / totalWords;

            int df = documentFrequency[entry.first];

            double idf = log(
                (totalDocuments + 1.0) /
                (df + 1.0)
            ) + 1.0;

            double weight = tf * idf;

            tfidfVectors[docId][entry.first] = weight;

            documentNorms[docId] +=
                weight * weight;
        }

        documentNorms[docId] =
            sqrt(documentNorms[docId]);
    }

    cout << "TF-IDF vectors generated successfully!\n";
}


// =====================================================
// PHASE 7: BM25 STATISTICS (content + title fields)
// =====================================================

void buildBM25Statistics() {

    int totalDocuments = static_cast<int>(documents.size());

    titleIndex.clear();
    contentLengths.assign(totalDocuments, 0);
    titleLengths.assign(totalDocuments, 0);

    double totalContentLength = 0.0;
    double totalTitleLength = 0.0;

    for (int docId = 0; docId < totalDocuments; docId++) {

        contentLengths[docId] = static_cast<int>(docTokens[docId].size());
        totalContentLength += docTokens[docId].size();

        titleLengths[docId] = static_cast<int>(titleTokens[docId].size());
        totalTitleLength += titleTokens[docId].size();

        for (const string& word : titleTokens[docId]) {
            titleIndex[word][docId]++;
        }
    }

    avgContentLength =
        totalDocuments > 0 ? totalContentLength / totalDocuments : 0.0;

    avgTitleLength =
        totalDocuments > 0 ? totalTitleLength / totalDocuments : 0.0;

    cout << "BM25 statistics generated successfully!\n";
}

// BM25 contribution of one term to one document in one field.
double bm25TermScore(
    const unordered_map<int, int>& postings,
    int docId,
    int fieldLength,
    double avgFieldLength,
    int totalDocuments
) {

    auto it = postings.find(docId);

    if (it == postings.end()) {
        return 0.0;
    }

    double tf = it->second;
    double df = static_cast<double>(postings.size());

    double idf = log(
        1.0 + (totalDocuments - df + 0.5) / (df + 0.5)
    );

    double lengthRatio =
        avgFieldLength > 0.0 ? fieldLength / avgFieldLength : 1.0;

    double norm = 1.0 - BM25_B + BM25_B * lengthRatio;

    return idf * (tf * (BM25_K1 + 1.0)) /
           (tf + BM25_K1 * norm);
}


// =====================================================
// COSINE SIMILARITY
// queryNorm is computed once per query (Phase 8 optimization)
// =====================================================

double calculateCosineSimilarity(
    const unordered_map<string, double>& queryVector,
    double queryNorm,
    int docId
) {

    if (queryNorm == 0.0 ||
        documentNorms[docId] == 0.0) {

        return 0.0;
    }

    double dotProduct = 0.0;

    for (const auto& entry : queryVector) {

        auto it = tfidfVectors[docId].find(entry.first);

        if (it != tfidfVectors[docId].end()) {

            dotProduct += entry.second * it->second;
        }
    }

    return dotProduct /
           (queryNorm * documentNorms[docId]);
}


// =====================================================
// SEARCH RESULT HIGHLIGHTING
// (content is lowercased once per call instead of per character)
// =====================================================

string highlightText(
    const string& content,
    const vector<string>& queryWords
) {

    string lowered = toLowerString(content);

    string result;
    result.reserve(content.size() + 16);

    for (size_t i = 0; i < content.size(); i++) {

        bool matched = false;

        for (const string& word : queryWords) {

            if (word.empty() ||
                i + word.size() > content.size()) {

                continue;
            }

            if (lowered.compare(i, word.size(), word) != 0) {
                continue;
            }

            bool leftBoundary =
                (i == 0 ||
                 !isalnum(static_cast<unsigned char>(
                     content[i - 1]
                 )));

            bool rightBoundary =
                (i + word.size() == content.size() ||
                 !isalnum(static_cast<unsigned char>(
                     content[i + word.size()]
                 )));

            if (leftBoundary && rightBoundary) {

                result += "**";
                result += content.substr(i, word.size());
                result += "**";

                i += word.size() - 1;

                matched = true;

                break;
            }
        }

        if (!matched) {
            result += content[i];
        }
    }

    return result;
}


// =====================================================
// PHASE 8.1: PERSISTENT SEARCH HISTORY
// File format: one raw query per line, oldest first.
// =====================================================

// Adds a query to memory only (used when loading from disk).
void addToMemoryHistory(const string& query) {

    searchHistory.push_back(query);

    string normalized = normalizeQuery(query);

    if (!normalized.empty()) {
        queryCounts[normalized]++;
    }
}

// Appends one query to the history file (crash-safe: written immediately).
void appendHistoryToFile(const string& query) {

    if (!historyEnabled || trimString(query).empty()) {
        return;
    }

    ofstream out(historyFile.c_str(), ios::app);

    if (!out.is_open()) {

        if (!historyWriteWarned) {

            cerr << "Warning: cannot write search history to "
                 << historyFile << "\n";

            historyWriteWarned = true;
        }

        return;
    }

    out << query << "\n";
}

// Records a query in memory and on disk.
void recordQuery(const string& query) {

    addToMemoryHistory(query);
    appendHistoryToFile(query);
}

// Loads previous history at startup. Returns the number of queries loaded.
int loadHistory() {

    ifstream in(historyFile.c_str());

    if (!in.is_open()) {
        return 0;
    }

    int loaded = 0;
    string line;

    while (getline(in, line)) {

        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }

        if (trimString(line).empty()) {
            continue;
        }

        addToMemoryHistory(line);
        loaded++;
    }

    return loaded;
}

void clearHistory() {

    searchHistory.clear();
    queryCounts.clear();

    ofstream out(historyFile.c_str(), ios::trunc);
}

void displaySearchHistory() {

    cout << "\n========== SEARCH HISTORY ==========" << endl;

    if (searchHistory.empty()) {
        cout << "No searches performed yet.\n";
        return;
    }

    const size_t maxShown = 50;

    size_t start =
        searchHistory.size() > maxShown ? searchHistory.size() - maxShown : 0;

    if (start > 0) {
        cout << "(showing the latest " << maxShown << " of "
             << searchHistory.size() << " searches)\n";
    }

    for (size_t i = start; i < searchHistory.size(); i++) {
        cout << i + 1 << ". " << searchHistory[i] << endl;
    }

    cout << "\nSaved in: " << historyFile << "\n";
}


// =====================================================
// PHASE 7: QUERY SUGGESTIONS (past queries by frequency)
// =====================================================

void suggestQueries(const string& prefix, int limit = 5) {

    string normalizedPrefix = normalizeQuery(prefix);

    vector<pair<string, int>> matches;

    for (const auto& entry : queryCounts) {

        if (entry.first.compare(0, normalizedPrefix.size(),
                                normalizedPrefix) == 0) {

            matches.push_back(entry);
        }
    }

    // Most frequent first, alphabetical on ties.
    sort(
        matches.begin(), matches.end(),
        [](const pair<string, int>& a, const pair<string, int>& b) {

            if (a.second != b.second) {
                return a.second > b.second;
            }

            return a.first < b.first;
        }
    );

    cout << "\n========== QUERY SUGGESTIONS ==========\n";

    if (matches.empty()) {

        cout << "No previous queries match \""
             << normalizedPrefix << "\".\n";

        return;
    }

    int count = min(limit, static_cast<int>(matches.size()));

    for (int i = 0; i < count; i++) {

        cout << i + 1 << ". " << matches[i].first
             << "  (searched " << matches[i].second
             << (matches[i].second == 1 ? " time)" : " times)")
             << "\n";
    }
}


// =====================================================
// PHASE 7: AUTOCORRECTION (LEVENSHTEIN DISTANCE)
// =====================================================

// Edit distance with an early exit once it must exceed maxDist.
int levenshteinDistance(
    const string& a,
    const string& b,
    int maxDist
) {

    int n = static_cast<int>(a.size());
    int m = static_cast<int>(b.size());

    if (abs(n - m) > maxDist) {
        return maxDist + 1;
    }

    vector<int> previous(m + 1);
    vector<int> current(m + 1);

    for (int j = 0; j <= m; j++) {
        previous[j] = j;
    }

    for (int i = 1; i <= n; i++) {

        current[0] = i;
        int rowMin = current[0];

        for (int j = 1; j <= m; j++) {

            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;

            current[j] = min(
                min(previous[j] + 1, current[j - 1] + 1),
                previous[j - 1] + cost
            );

            rowMin = min(rowMin, current[j]);
        }

        if (rowMin > maxDist) {
            return maxDist + 1;
        }

        swap(previous, current);
    }

    return previous[m];
}

// Best vocabulary match for a word, or "" if nothing is close enough.
string findClosestWord(const string& word) {

    int maxDist;

    if (word.size() <= 2) {
        return "";
    }
    else if (word.size() <= 5) {
        maxDist = 1;
    }
    else {
        maxDist = 2;
    }

    string best;
    int bestDist = maxDist + 1;
    int bestFrequency = -1;

    for (const auto& entry : corpusTermFrequency) {

        int dist = levenshteinDistance(word, entry.first, maxDist);

        if (dist > maxDist) {
            continue;
        }

        bool better =
            dist < bestDist ||
            (dist == bestDist && entry.second > bestFrequency) ||
            (dist == bestDist && entry.second == bestFrequency &&
             entry.first < best);

        if (better) {
            best = entry.first;
            bestDist = dist;
            bestFrequency = entry.second;
        }
    }

    return best;
}

// Replaces unknown words with their closest vocabulary word and
// reports the change to the user.
vector<string> applyAutocorrection(const vector<string>& words) {

    vector<string> corrected;
    vector<pair<string, string>> changes;

    for (const string& word : words) {

        bool known = corpusTermFrequency.count(word) > 0;

        bool hasDigit = false;
        for (unsigned char c : word) {
            if (isdigit(c)) {
                hasDigit = true;
                break;
            }
        }

        if (known || hasDigit) {
            corrected.push_back(word);
            continue;
        }

        string match = findClosestWord(word);

        if (match.empty()) {
            corrected.push_back(word);
        }
        else {
            corrected.push_back(match);
            changes.push_back({word, match});
        }
    }

    if (!changes.empty()) {

        cout << "\nDid you mean: ";

        for (size_t i = 0; i < corrected.size(); i++) {
            cout << corrected[i] << (i + 1 < corrected.size() ? " " : "");
        }

        cout << "  (showing results for the corrected query)\n";

        for (const auto& change : changes) {
            cout << "  " << change.first << " -> " << change.second << "\n";
        }
    }

    return corrected;
}


// =====================================================
// RANKING FUNCTIONS
// limit > 0 keeps only the best `limit` results (partial sort);
// limit <= 0 returns every match, fully sorted.
// =====================================================

bool rankedBefore(
    const pair<int, double>& a,
    const pair<int, double>& b
) {

    if (a.second != b.second) {
        return a.second > b.second;
    }

    return a.first < b.first;
}

void sortRanked(RankedList& results, int limit) {

    if (limit > 0 && limit < static_cast<int>(results.size())) {

        partial_sort(
            results.begin(), results.begin() + limit,
            results.end(), rankedBefore
        );

        results.resize(limit);
    }
    else {
        sort(results.begin(), results.end(), rankedBefore);
    }
}

RankedList rankTFIDF(const vector<string>& queryWords, int limit) {

    RankedList results;

    if (queryWords.empty()) {
        return results;
    }

    unordered_map<string, int> queryFrequency;
    for (const string& word : queryWords) {
        queryFrequency[word]++;
    }

    unordered_map<string, double> queryVector;
    int totalDocuments = static_cast<int>(documents.size());
    double queryNorm = 0.0;

    for (const auto& entry : queryFrequency) {
        const string& word = entry.first;
        int frequency = entry.second;

        double tf = static_cast<double>(frequency) / queryWords.size();
        int df = documentFrequency.count(word) ? documentFrequency.at(word) : 0;
        double idf = log((totalDocuments + 1.0) / (df + 1.0)) + 1.0;

        double weight = tf * idf;
        queryVector[word] = weight;
        queryNorm += weight * weight;
    }

    queryNorm = sqrt(queryNorm);

    // Use the inverted index to identify only documents containing query terms.
    unordered_set<int> candidateDocuments;
    for (const auto& entry : queryFrequency) {
        auto postingList = invertedIndex.find(entry.first);
        if (postingList != invertedIndex.end()) {
            for (const auto& posting : postingList->second) {
                candidateDocuments.insert(posting.first);
            }
        }
    }

    results.reserve(candidateDocuments.size());

    for (int docId : candidateDocuments) {
        double score = calculateCosineSimilarity(queryVector, queryNorm, docId);
        if (score > 0.0) {
            results.push_back({docId, score});
        }
    }

    sortRanked(results, limit);

    return results;
}

RankedList rankBM25(
    const vector<string>& queryWords,
    Field field,
    int limit
) {

    int totalDocuments = static_cast<int>(documents.size());

    vector<const unordered_map<int, int>*> contentPostings;
    vector<const unordered_map<int, int>*> titlePostings;
    unordered_set<int> candidateDocuments;

    for (const string& word : queryWords) {

        if (field != FIELD_TITLE) {

            auto it = invertedIndex.find(word);

            if (it != invertedIndex.end()) {

                contentPostings.push_back(&it->second);

                for (const auto& posting : it->second) {
                    candidateDocuments.insert(posting.first);
                }
            }
        }

        if (field != FIELD_CONTENT) {

            auto it = titleIndex.find(word);

            if (it != titleIndex.end()) {

                titlePostings.push_back(&it->second);

                for (const auto& posting : it->second) {
                    candidateDocuments.insert(posting.first);
                }
            }
        }
    }

    double titleWeight = (field == FIELD_BOTH) ? TITLE_BOOST : 1.0;

    RankedList results;
    results.reserve(candidateDocuments.size());

    for (int docId : candidateDocuments) {

        double score = 0.0;

        for (size_t i = 0; i < contentPostings.size(); i++) {
            score += bm25TermScore(
                *contentPostings[i], docId,
                contentLengths[docId], avgContentLength,
                totalDocuments
            );
        }

        for (size_t i = 0; i < titlePostings.size(); i++) {
            score += titleWeight * bm25TermScore(
                *titlePostings[i], docId,
                titleLengths[docId], avgTitleLength,
                totalDocuments
            );
        }

        if (score > 0.0) {
            results.push_back({docId, score});
        }
    }

    sortRanked(results, limit);

    return results;
}


// =====================================================
// PHASE 8.3: RESULT CACHE
// Key = mode + sorted query terms + topK. The corpus is static
// while the program runs, so cached results never go stale.
// =====================================================

string makeCacheKey(
    const string& mode,
    vector<string> words,
    int topK
) {

    sort(words.begin(), words.end());

    string key = mode + "|";

    for (const string& word : words) {
        key += word + " ";
    }

    return key + "|" + intToString(topK);
}

bool cacheLookup(const string& key, RankedList& out) {

    if (!cacheEnabled) {
        return false;
    }

    auto it = resultCache.find(key);

    if (it == resultCache.end()) {
        cacheMisses++;
        return false;
    }

    cacheHits++;
    out = it->second;

    return true;
}

void cacheStore(const string& key, const RankedList& results) {

    if (!cacheEnabled) {
        return;
    }

    if (resultCache.size() >= CACHE_LIMIT) {
        resultCache.clear();
    }

    resultCache[key] = results;
}

RankedList runTFIDF(const vector<string>& words, int topK) {

    string key = makeCacheKey("tfidf", words, topK);

    RankedList results;

    if (cacheLookup(key, results)) {
        return results;
    }

    results = rankTFIDF(words, topK);
    cacheStore(key, results);

    return results;
}

RankedList runBM25(const vector<string>& words, Field field, int topK) {

    string key = makeCacheKey(
        "bm25-" + intToString(static_cast<int>(field)), words, topK
    );

    RankedList results;

    if (cacheLookup(key, results)) {
        return results;
    }

    results = rankBM25(words, field, topK);
    cacheStore(key, results);

    return results;
}


// =====================================================
// RESULT PRINTING
// =====================================================

void printResults(
    const RankedList& results,
    int topK,
    const vector<string>& queryWords,
    const string& heading
) {

    cout << "\n========== " << heading
         << " (TOP " << topK << ") ==========" << endl;

    if (results.empty()) {
        cout << "No relevant documents found.\n";
        return;
    }

    int resultCount = min(topK, static_cast<int>(results.size()));

    for (int i = 0; i < resultCount; i++) {
        int docId = results[i].first;
        double score = results[i].second;

        cout << "\nRank: " << i + 1;
        cout << "\nDocument ID: " << docId + 1;
        cout << "\nTitle: " << highlightText(documentTitles[docId], queryWords);
        cout << "\nFile: " << documentNames[docId];
        cout << "\nRelevance Score: " << fixed << setprecision(4) << score;
        cout << "\nContent: "
             << highlightText(documents[docId].substr(0, 200), queryWords)
             << "\n-----------------------------------\n";
    }
}

void printElapsed(
    chrono::high_resolution_clock::time_point startTime,
    bool fromCache
) {

    auto endTime = chrono::high_resolution_clock::now();
    double elapsedMs = chrono::duration<double, milli>(endTime - startTime).count();

    cout << "\nSearch execution time: " << fixed << setprecision(3)
         << elapsedMs << " ms"
         << (fromCache ? " (served from cache)" : "") << "\n";
}


// =====================================================
// TF-IDF SEARCH
// =====================================================

void searchDocuments(const string& query, int topK = 5) {

    auto startTime = chrono::high_resolution_clock::now();

    vector<string> queryWords = tokenize(query);

    if (queryWords.empty()) {
        cout << "\nPlease enter a valid search query.\n";
        return;
    }

    recordQuery(query);

    queryWords = applyAutocorrection(queryWords);

    long hitsBefore = cacheHits;

    RankedList results = runTFIDF(queryWords, topK);

    bool fromCache = cacheHits > hitsBefore;

    printResults(results, topK, queryWords, "TF-IDF SEARCH RESULTS");

    printElapsed(startTime, fromCache);
}


// =====================================================
// BM25 SEARCH (WITH FIELD SELECTION)
// =====================================================

void searchBM25(const string& query, Field field, int topK = 5) {

    auto startTime = chrono::high_resolution_clock::now();

    vector<string> queryWords = tokenize(query);

    if (queryWords.empty()) {
        cout << "\nPlease enter a valid search query.\n";
        return;
    }

    recordQuery(query);

    queryWords = applyAutocorrection(queryWords);

    long hitsBefore = cacheHits;

    RankedList results = runBM25(queryWords, field, topK);

    bool fromCache = cacheHits > hitsBefore;

    printResults(
        results, topK, queryWords,
        "BM25 RESULTS - " + fieldName(field)
    );

    printElapsed(startTime, fromCache);
}


// =====================================================
// TF-IDF vs BM25 COMPARISON
// =====================================================

void compareRankings(const string& query, int topK = 5) {

    vector<string> queryWords = tokenize(query);

    if (queryWords.empty()) {
        cout << "\nPlease enter a valid search query.\n";
        return;
    }

    recordQuery(query);

    queryWords = applyAutocorrection(queryWords);

    // Full (unlimited) lists so ranks outside the top K are known.
    auto t1 = chrono::high_resolution_clock::now();
    RankedList tfidfResults = rankTFIDF(queryWords, 0);
    auto t2 = chrono::high_resolution_clock::now();
    RankedList bm25Results = rankBM25(queryWords, FIELD_CONTENT, 0);
    auto t3 = chrono::high_resolution_clock::now();

    double tfidfMs = chrono::duration<double, milli>(t2 - t1).count();
    double bm25Ms = chrono::duration<double, milli>(t3 - t2).count();

    cout << "\n========== TF-IDF vs BM25 (TOP " << topK
         << ", content field) ==========\n";

    if (tfidfResults.empty() && bm25Results.empty()) {
        cout << "No relevant documents found.\n";
        return;
    }

    int tfidfCount = min(topK, static_cast<int>(tfidfResults.size()));
    int bm25Count = min(topK, static_cast<int>(bm25Results.size()));
    int rows = max(tfidfCount, bm25Count);

    cout << "\n" << left
         << setw(6) << "Rank"
         << setw(28) << "TF-IDF (Doc / score)"
         << setw(28) << "BM25 (Doc / score)" << "\n";

    cout << string(62, '-') << "\n";

    for (int i = 0; i < rows; i++) {

        ostringstream leftCell;
        ostringstream rightCell;

        if (i < tfidfCount) {
            leftCell << "Doc " << tfidfResults[i].first + 1
                     << " / " << fixed << setprecision(4)
                     << tfidfResults[i].second;
        }
        else {
            leftCell << "-";
        }

        if (i < bm25Count) {
            rightCell << "Doc " << bm25Results[i].first + 1
                      << " / " << fixed << setprecision(4)
                      << bm25Results[i].second;
        }
        else {
            rightCell << "-";
        }

        cout << left << setw(6) << i + 1
             << setw(28) << leftCell.str()
             << setw(28) << rightCell.str() << "\n";
    }

    unordered_map<int, int> tfidfRank;
    unordered_map<int, int> bm25Rank;

    for (size_t i = 0; i < tfidfResults.size(); i++) {
        tfidfRank[tfidfResults[i].first] = static_cast<int>(i) + 1;
    }

    for (size_t i = 0; i < bm25Results.size(); i++) {
        bm25Rank[bm25Results[i].first] = static_cast<int>(i) + 1;
    }

    vector<int> shown;
    unordered_set<int> seen;

    for (int i = 0; i < tfidfCount; i++) {
        int id = tfidfResults[i].first;
        if (seen.insert(id).second) shown.push_back(id);
    }

    for (int i = 0; i < bm25Count; i++) {
        int id = bm25Results[i].first;
        if (seen.insert(id).second) shown.push_back(id);
    }

    int overlap = 0;

    cout << "\nRank of each document in both lists:\n";

    for (int id : shown) {

        bool inTfidf = tfidfRank.count(id) > 0;
        bool inBm25 = bm25Rank.count(id) > 0;

        if (inTfidf && inBm25 &&
            tfidfRank[id] <= topK && bm25Rank[id] <= topK) {
            overlap++;
        }

        cout << "  Doc " << id + 1 << ": TF-IDF #";

        if (inTfidf) cout << tfidfRank[id]; else cout << "-";

        cout << ", BM25 #";

        if (inBm25) cout << bm25Rank[id]; else cout << "-";

        cout << "\n";
    }

    cout << "\nDocuments in both top-" << topK << " lists: " << overlap << "\n";

    if (tfidfCount > 0 && bm25Count > 0) {

        bool agree = tfidfResults[0].first == bm25Results[0].first;

        cout << "Top result: "
             << (agree ? "both methods agree (Doc "
                       : "methods disagree (TF-IDF: Doc ")
             << tfidfResults[0].first + 1;

        if (!agree) {
            cout << ", BM25: Doc " << bm25Results[0].first + 1;
        }

        cout << ")\n";
    }

    cout << "TF-IDF time: " << fixed << setprecision(3) << tfidfMs
         << " ms | BM25 time: " << bm25Ms << " ms\n";
}


// =====================================================
// PHASE 5.1: BOOLEAN SEARCH
// =====================================================

unordered_set<int> getDocumentsForTerm(
    const string& word
) {

    unordered_set<int> result;

    auto it = invertedIndex.find(word);

    if (it != invertedIndex.end()) {

        for (const auto& posting : it->second) {
            result.insert(posting.first);
        }
    }

    return result;
}


unordered_set<int> booleanSearch(
    const string& query
) {

    stringstream ss(query);

    vector<string> tokens;
    string token;

    while (ss >> token) {

        transform(
            token.begin(),
            token.end(),
            token.begin(),
            [](unsigned char c) {
                return static_cast<char>(toupper(c));
            }
        );

        tokens.push_back(token);
    }

    if (tokens.empty()) {
        return unordered_set<int>();
    }

    if (tokens.size() % 2 == 0) {
        cout << "Invalid Boolean query.\n";
        return unordered_set<int>();
    }

    unordered_set<int> result =
        getDocumentsForTerm(
            toLowerString(tokens[0])
        );

    for (int i = 1;
         i < static_cast<int>(tokens.size());
         i += 2) {

        if (i + 1 >= static_cast<int>(tokens.size())) {

            cout << "Invalid Boolean query.\n";

            return unordered_set<int>();
        }

        string operation = tokens[i];

        unordered_set<int> next =
            getDocumentsForTerm(
                toLowerString(tokens[i + 1])
            );

        if (operation == "AND") {

            unordered_set<int> intersection;

            for (int id : result) {

                if (next.count(id)) {
                    intersection.insert(id);
                }
            }

            result = intersection;
        }

        else if (operation == "OR") {

            for (int id : next) {
                result.insert(id);
            }
        }

        else if (operation == "NOT") {

            for (int id : next) {
                result.erase(id);
            }
        }

        else {

            cout << "Invalid operator: "
                 << operation << endl;

            return unordered_set<int>();
        }
    }

    return result;
}


// =====================================================
// PHASE 5.2: PHRASE SEARCH
// (documents are normalized once at build time)
// =====================================================

string normalizePhrase(const string& text) {

    string cleaned = preprocess(text);

    stringstream ss(cleaned);

    string word;
    string result;

    while (ss >> word) {

        if (!result.empty()) {
            result += " ";
        }

        result += word;
    }

    return result;
}

// Both arguments must already be normalized with normalizePhrase().
bool containsNormalizedPhrase(
    const string& normalizedDocument,
    const string& normalizedPhrase
) {

    if (normalizedPhrase.empty()) {
        return false;
    }

    size_t position =
        normalizedDocument.find(normalizedPhrase);

    while (position != string::npos) {

        bool leftBoundary =
            (position == 0 ||
             normalizedDocument[position - 1] == ' ');

        size_t endPosition =
            position + normalizedPhrase.size();

        bool rightBoundary =
            (endPosition == normalizedDocument.size() ||
             normalizedDocument[endPosition] == ' ');

        if (leftBoundary && rightBoundary) {
            return true;
        }

        position = normalizedDocument.find(
            normalizedPhrase,
            position + 1
        );
    }

    return false;
}


// =====================================================
// PHASE 5.3: ADVANCED SEARCH
// =====================================================

void advancedSearch(const string& query) {

    auto startTime = chrono::high_resolution_clock::now();

    if (query.empty()) {

        cout << "\nSearch query cannot be empty.\n";

        return;
    }

    // Phrase Search
    if (query.size() >= 2 &&
        query.front() == '"' &&
        query.back() == '"') {

        recordQuery(query);

        string phrase =
            query.substr(1, query.size() - 2);

        vector<string> phraseWords =
            tokenize(phrase);

        string normalizedPhrase = normalizePhrase(phrase);

        cout << "\n========== PHRASE SEARCH ==========\n";

        int found = 0;

        for (int i = 0;
             i < static_cast<int>(documents.size());
             i++) {

            if (containsNormalizedPhrase(normalizedDocuments[i],
                                         normalizedPhrase)) {

                found++;

                cout << "\nDocument ID: " << i + 1;

                cout << "\nTitle: " << documentTitles[i];

                cout << "\nFile: "
                     << documentNames[i];

                cout << "\nContent: "
                     << highlightText(
                            documents[i],
                            phraseWords
                        )
                     << "\n";
            }
        }

        if (found == 0) {
            cout << "No exact phrase found.\n";
        }

        printElapsed(startTime, false);

        return;
    }

    // Boolean Search
    string upperQuery = query;

    transform(
        upperQuery.begin(),
        upperQuery.end(),
        upperQuery.begin(),
        [](unsigned char c) {
            return static_cast<char>(toupper(c));
        }
    );

    if (upperQuery.find(" AND ") != string::npos ||
        upperQuery.find(" OR ") != string::npos ||
        upperQuery.find(" NOT ") != string::npos) {

        recordQuery(query);

        unordered_set<int> results =
            booleanSearch(query);

        cout << "\n========== BOOLEAN SEARCH ==========\n";

        if (results.empty()) {

            cout << "No matching documents found.\n";

            return;
        }

        vector<int> sortedResults(
            results.begin(),
            results.end()
        );

        sort(
            sortedResults.begin(),
            sortedResults.end()
        );

        vector<string> queryWords =
            tokenize(query);

        for (size_t i = 0; i < queryWords.size(); i++) {
            // Operators are never highlighted.
            if (queryWords[i] == "not") {
                queryWords.erase(queryWords.begin() + i);
                i--;
            }
        }

        for (int docId : sortedResults) {

            cout << "\nDocument ID: "
                 << docId + 1;

            cout << "\nTitle: "
                 << documentTitles[docId];

            cout << "\nFile: "
                 << documentNames[docId];

            cout << "\nContent: "
                 << highlightText(
                        documents[docId],
                        queryWords
                    )
                 << "\n";
        }

        printElapsed(startTime, false);

        return;
    }

    // Normal search (TF-IDF with autocorrection)
    searchDocuments(query);
}


// =====================================================
// PHASE 8.2: COMBINED AUTOCOMPLETE
// Past queries (by frequency) + Trie vocabulary (by corpus frequency).
// =====================================================

struct Suggestion {

    string text;
    bool fromHistory;
    int score;      // times searched (history) or corpus count (vocabulary)
};

vector<Suggestion> getCombinedSuggestions(
    const string& query,
    int limit
) {

    vector<Suggestion> merged;

    if (limit <= 0) {
        return merged;
    }

    string normalized = normalizeQuery(query);

    if (normalized.empty()) {
        return merged;
    }

    bool trailingSpace =
        isspace(static_cast<unsigned char>(query[query.size() - 1])) != 0;

    string historyPrefix = normalized + (trailingSpace ? " " : "");

    // 1. Past queries starting with what the user has typed.
    vector<pair<string, int>> historyMatches;

    for (const auto& entry : queryCounts) {

        if (entry.first.size() >= historyPrefix.size() &&
            entry.first.compare(0, historyPrefix.size(), historyPrefix) == 0 &&
            entry.first != normalized) {

            historyMatches.push_back(entry);
        }
    }

    sort(
        historyMatches.begin(), historyMatches.end(),
        [](const pair<string, int>& a, const pair<string, int>& b) {

            if (a.second != b.second) {
                return a.second > b.second;
            }

            return a.first < b.first;
        }
    );

    // 2. Trie completions for the word being typed.
    vector<Suggestion> trieSuggestions;

    if (!trailingSpace) {

        stringstream ss(preprocess(query));

        vector<string> words;
        string word;

        while (ss >> word) {
            words.push_back(word);
        }

        if (!words.empty()) {

            string previousWords;

            for (size_t i = 0; i + 1 < words.size(); i++) {
                previousWords += words[i] + " ";
            }

            vector<string> completions =
                autocompleteTrie.autocomplete(words.back(), limit);

            for (const string& completion : completions) {

                auto it = corpusTermFrequency.find(completion);

                Suggestion s;
                s.text = previousWords + completion;
                s.fromHistory = false;
                s.score = (it != corpusTermFrequency.end()) ? it->second : 0;

                trieSuggestions.push_back(s);
            }
        }
    }

    // 3. Merge: up to half from history first, then vocabulary,
    //    then any remaining history, without duplicates.
    unordered_set<string> seen;

    int historyQuota = (limit + 1) / 2;
    size_t historyIndex = 0;

    while (historyIndex < historyMatches.size() &&
           static_cast<int>(merged.size()) < historyQuota) {

        Suggestion s;
        s.text = historyMatches[historyIndex].first;
        s.fromHistory = true;
        s.score = historyMatches[historyIndex].second;

        if (seen.insert(s.text).second) {
            merged.push_back(s);
        }

        historyIndex++;
    }

    for (size_t i = 0;
         i < trieSuggestions.size() &&
         static_cast<int>(merged.size()) < limit;
         i++) {

        if (seen.insert(trieSuggestions[i].text).second) {
            merged.push_back(trieSuggestions[i]);
        }
    }

    while (historyIndex < historyMatches.size() &&
           static_cast<int>(merged.size()) < limit) {

        Suggestion s;
        s.text = historyMatches[historyIndex].first;
        s.fromHistory = true;
        s.score = historyMatches[historyIndex].second;

        if (seen.insert(s.text).second) {
            merged.push_back(s);
        }

        historyIndex++;
    }

    return merged;
}

void autocompleteQuery(const string& query, int limit = 5) {

    cout << "\n========== AUTOCOMPLETE ==========\n";

    if (normalizeQuery(query).empty()) {

        cout << "Enter a prefix to get suggestions.\n";

        return;
    }

    vector<Suggestion> suggestions =
        getCombinedSuggestions(query, limit);

    if (suggestions.empty()) {

        cout << "No suggestions found.\n";

        return;
    }

    for (size_t i = 0; i < suggestions.size(); i++) {

        cout << i + 1 << ". " << suggestions[i].text << "   ";

        if (suggestions[i].fromHistory) {

            cout << "[history: searched " << suggestions[i].score
                 << (suggestions[i].score == 1 ? " time]" : " times]");
        }
        else {

            cout << "[vocabulary: " << suggestions[i].score
                 << (suggestions[i].score == 1 ? " occurrence]"
                                               : " occurrences]");
        }

        cout << "\n";
    }
}


// =====================================================
// ENGINE BUILD (tokenize once, then build every index)
// =====================================================

void buildEngine() {

    auto startTime = chrono::high_resolution_clock::now();

    int totalDocuments = static_cast<int>(documents.size());

    docTokens.assign(totalDocuments, vector<string>());
    titleTokens.assign(totalDocuments, vector<string>());
    normalizedDocuments.assign(totalDocuments, string());

    for (int i = 0; i < totalDocuments; i++) {

        docTokens[i] = tokenize(documents[i]);
        titleTokens[i] = tokenize(titleSearchText[i]);
        normalizedDocuments[i] = normalizePhrase(documents[i]);
    }

    buildInvertedIndex();
    buildTFIDF();
    buildBM25Statistics();

    auto endTime = chrono::high_resolution_clock::now();

    indexBuildMs =
        chrono::duration<double, milli>(endTime - startTime).count();
}


// =====================================================
// PHASE 9.1: SYSTEM DIAGNOSTICS
// =====================================================

void displayDiagnostics() {

    long totalTokens = 0;
    long totalPostings = 0;
    int emptyDocuments = 0;
    int longestDocument = 0;

    for (size_t i = 0; i < docTokens.size(); i++) {

        totalTokens += static_cast<long>(docTokens[i].size());

        if (docTokens[i].empty()) {
            emptyDocuments++;
        }

        longestDocument =
            max(longestDocument, static_cast<int>(docTokens[i].size()));
    }

    for (const auto& entry : invertedIndex) {
        totalPostings += static_cast<long>(entry.second.size());
    }

    bool historyWritable = false;

    {
        ofstream probe(historyFile.c_str(), ios::app);
        historyWritable = probe.is_open();
    }

    cout << "\n========== SYSTEM DIAGNOSTICS ==========\n";

    cout << left;

    cout << setw(28) << "Data folder:" << dataFolder << "\n";
    cout << setw(28) << "History file:" << historyFile
         << (historyWritable ? " (writable)" : " (NOT WRITABLE)") << "\n";

    cout << "\n-- Corpus --\n";
    cout << setw(28) << "Documents:" << documents.size() << "\n";
    cout << setw(28) << "Total tokens:" << totalTokens << "\n";
    cout << setw(28) << "Vocabulary (unique terms):" << corpusTermFrequency.size() << "\n";
    cout << setw(28) << "Title vocabulary:" << titleIndex.size() << "\n";
    cout << setw(28) << "Index postings:" << totalPostings << "\n";
    cout << setw(28) << "Longest document (tokens):" << longestDocument << "\n";
    cout << fixed << setprecision(2);
    cout << setw(28) << "Avg content length:" << avgContentLength << " tokens\n";
    cout << setw(28) << "Avg title length:" << avgTitleLength << " tokens\n";

    cout << "\n-- Performance --\n";
    cout << setprecision(3);
    cout << setw(28) << "Index build time:" << indexBuildMs << " ms\n";

    cout << "\n-- Queries --\n";
    cout << setw(28) << "Searches this + past runs:" << searchHistory.size() << "\n";
    cout << setw(28) << "Unique queries:" << queryCounts.size() << "\n";
    cout << setw(28) << "Cache entries:" << resultCache.size()
         << " / " << CACHE_LIMIT << "\n";
    cout << setw(28) << "Cache hits / misses:" << cacheHits << " / " << cacheMisses << "\n";

    cout << "\n-- Health checks --\n";

    bool healthy = true;

    if (emptyDocuments > 0) {
        cout << "WARNING: " << emptyDocuments
             << " document(s) have no searchable words.\n";
        healthy = false;
    }

    if (!historyWritable) {
        cout << "WARNING: search history cannot be saved.\n";
        healthy = false;
    }

    if (documents.size() < 3) {
        cout << "NOTE: very few documents; rankings will look alike.\n";
    }

    if (healthy) {
        cout << "All checks passed.\n";
    }
}


// =====================================================
// PHASE 9.2: PERFORMANCE BENCHMARK
// =====================================================

void runBenchmark(int numQueries = 1000) {

    vector<string> vocabulary;

    for (const auto& entry : corpusTermFrequency) {
        vocabulary.push_back(entry.first);
    }

    if (vocabulary.empty()) {
        cout << "\nNothing to benchmark.\n";
        return;
    }

    // Sorted so the generated queries are identical on every run.
    sort(vocabulary.begin(), vocabulary.end());

    unsigned long long seed = 12345ULL;

    vector<vector<string>> queries;

    for (int i = 0; i < numQueries; i++) {

        vector<string> query;

        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        int termCount = 1 + static_cast<int>((seed >> 33) % 3);

        for (int t = 0; t < termCount; t++) {

            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            query.push_back(vocabulary[(seed >> 33) % vocabulary.size()]);
        }

        queries.push_back(query);
    }

    cout << "\n========== BENCHMARK (" << numQueries
         << " random queries, top 5, cache bypassed) ==========\n";

    cout << fixed << setprecision(1);

    size_t checksum = 0;   // Prevents the work from being optimized away

    auto timeIt = [&](const string& label, int mode) {

        auto start = chrono::high_resolution_clock::now();

        for (size_t i = 0; i < queries.size(); i++) {

            RankedList results;

            if (mode == 0) {
                results = rankTFIDF(queries[i], 5);
            }
            else if (mode == 1) {
                results = rankBM25(queries[i], FIELD_CONTENT, 5);
            }
            else {
                results = rankBM25(queries[i], FIELD_BOTH, 5);
            }

            checksum += results.size();
        }

        auto end = chrono::high_resolution_clock::now();

        double totalMs = chrono::duration<double, milli>(end - start).count();

        cout << left << setw(22) << label
             << "total " << setw(10) << totalMs << "ms | average "
             << (totalMs * 1000.0 / queries.size()) << " us/query\n";
    };

    timeIt("TF-IDF (cosine):", 0);
    timeIt("BM25 (content):", 1);
    timeIt("BM25 (title+content):", 2);

    // Autocorrection cost on misspelled terms.
    {
        auto start = chrono::high_resolution_clock::now();

        int corrections = 0;

        for (int i = 0; i < 100; i++) {

            string word = vocabulary[(i * 7) % vocabulary.size()];

            if (word.size() > 3) {
                word[1] = (word[1] == 'z') ? 'y' : 'z';
            }

            if (!findClosestWord(word).empty()) {
                corrections++;
            }
        }

        auto end = chrono::high_resolution_clock::now();

        double totalMs = chrono::duration<double, milli>(end - start).count();

        cout << left << setw(22) << "Autocorrect (100):"
             << "total " << setw(10) << totalMs << "ms | average "
             << (totalMs * 1000.0 / 100) << " us/word ("
             << corrections << " corrected)\n";
    }

    cout << "\nVocabulary size: " << vocabulary.size()
         << " terms, documents: " << documents.size()
         << ", index build time: " << setprecision(3) << indexBuildMs << " ms\n";

    (void)checksum;
}


// =====================================================
// PHASE 9.3: AUTOMATED SELF-TESTS  (run with --selftest)
// Uses a built-in 4-document corpus, so results do not
// depend on the files in the Data folder.
// =====================================================

int testsPassed = 0;
int testsFailed = 0;

void check(const string& name, bool condition) {

    cout << (condition ? "[PASS] " : "[FAIL] ") << name << "\n";

    if (condition) {
        testsPassed++;
    }
    else {
        testsFailed++;
    }
}

vector<int> idsOf(const RankedList& results) {

    vector<int> ids;

    for (size_t i = 0; i < results.size(); i++) {
        ids.push_back(results[i].first);
    }

    return ids;
}

vector<int> sortedIds(const unordered_set<int>& set) {

    vector<int> ids(set.begin(), set.end());
    sort(ids.begin(), ids.end());

    return ids;
}

vector<int> makeIds(int a, int b = -1, int c = -1) {

    vector<int> ids;

    ids.push_back(a);
    if (b >= 0) ids.push_back(b);
    if (c >= 0) ids.push_back(c);

    return ids;
}

bool containsSuggestion(const vector<Suggestion>& list, const string& text) {

    for (size_t i = 0; i < list.size(); i++) {
        if (list[i].text == text) {
            return true;
        }
    }

    return false;
}

int runSelfTests() {

    cout << "====================================\n";
    cout << "        SEARCH ENGINE SELF-TEST     \n";
    cout << "====================================\n\n";

    struct TestDoc {
        const char* title;
        const char* body;
    };

    TestDoc corpus[] = {
        { "Machine Learning Basics",
          "Machine learning is a field of artificial intelligence. "
          "Machine learning algorithms learn patterns from data." },
        { "Search Engines",
          "A search engine uses an inverted index to retrieve documents. "
          "Ranking uses TF-IDF and BM25." },
        { "Cooking Pasta",
          "Boil water, add salt, cook pasta for ten minutes. "
          "Pasta sauce can be made with tomatoes." },
        { "Data Structures",
          "A trie is a tree used for autocomplete. "
          "An inverted index maps terms to documents." }
    };

    for (int i = 0; i < 4; i++) {

        string name = "mem" + intToString(i + 1) + ".txt";

        addDocumentText(
            name, corpus[i].title,
            string(corpus[i].title) + " " + corpus[i].body + " "
        );
    }

    {
        OutputSilencer quiet;
        buildEngine();
    }

    cout << "--- Preprocessing & indexing ---\n";

    {
        vector<string> tokens = tokenize("The Quick, brown!");
        check("tokenize lowercases, strips punctuation and stop words",
              tokens.size() == 2 && tokens[0] == "quick" && tokens[1] == "brown");
    }

    check("normalizeQuery trims, lowercases, collapses spaces",
          normalizeQuery("  Hello   WORLD ") == "hello world");

    check("4 documents loaded", documents.size() == 4);
    check("inverted index: 'pasta' appears in exactly 1 document",
          documentFrequency["pasta"] == 1);
    check("inverted index: 'index' appears in 2 documents",
          documentFrequency["index"] == 2);
    check("TF-IDF document norms are positive",
          documentNorms[0] > 0 && documentNorms[1] > 0 &&
          documentNorms[2] > 0 && documentNorms[3] > 0);
    check("title index built (title 'search' -> doc 2 only)",
          titleIndex["search"].size() == 1 && titleIndex["search"].count(1) == 1);

    cout << "\n--- TF-IDF ranking ---\n";

    {
        RankedList r = rankTFIDF(tokenize("pasta"), 5);
        check("TF-IDF 'pasta' -> only the cooking document",
              r.size() == 1 && r[0].first == 2);
    }

    {
        RankedList r = rankTFIDF(tokenize("machine learning"), 5);
        check("TF-IDF 'machine learning' ranks doc 1 first",
              !r.empty() && r[0].first == 0);
        check("TF-IDF cosine scores stay within (0, 1]",
              !r.empty() && r[0].second > 0.0 && r[0].second <= 1.0000001);
    }

    {
        RankedList r = rankTFIDF(tokenize("zzzzunknown"), 5);
        check("TF-IDF unknown word -> no results", r.empty());
    }

    cout << "\n--- BM25 ranking & fields ---\n";

    {
        RankedList r = rankBM25(tokenize("bm25"), FIELD_CONTENT, 5);
        check("BM25 'bm25' -> doc 2 (search engines)",
              r.size() == 1 && r[0].first == 1);
    }

    {
        RankedList r = rankBM25(tokenize("inverted index"), FIELD_CONTENT, 5);
        vector<int> ids = idsOf(r);
        sort(ids.begin(), ids.end());
        check("BM25 'inverted index' -> docs 2 and 4",
              ids == makeIds(1, 3));
        check("BM25 scores are positive and sorted best-first",
              r.size() == 2 && r[0].second > 0 && r[0].second >= r[1].second);
    }

    {
        RankedList r = rankBM25(tokenize("pasta"), FIELD_TITLE, 5);
        check("BM25 title field 'pasta' -> doc 3",
              r.size() == 1 && r[0].first == 2);
    }

    {
        RankedList r = rankBM25(tokenize("search"), FIELD_TITLE, 5);
        check("BM25 title field 'search' -> doc 2",
              r.size() == 1 && r[0].first == 1);
    }

    {
        RankedList content = rankBM25(tokenize("pasta"), FIELD_CONTENT, 5);
        RankedList both = rankBM25(tokenize("pasta"), FIELD_BOTH, 5);
        check("BM25 'both' mode scores title+content higher than content alone",
              !both.empty() && !content.empty() &&
              both[0].second > content[0].second);
    }

    {
        RankedList limited = rankBM25(tokenize("documents"), FIELD_CONTENT, 1);
        RankedList full = rankBM25(tokenize("documents"), FIELD_CONTENT, 0);
        check("top-K partial sort matches the head of the full sort",
              limited.size() == 1 && !full.empty() &&
              limited[0].first == full[0].first);
    }

    cout << "\n--- Autocorrection (Levenshtein) ---\n";

    check("levenshtein('kitten','sitting') = 3",
          levenshteinDistance("kitten", "sitting", 10) == 3);
    check("levenshtein of identical words = 0",
          levenshteinDistance("pasta", "pasta", 2) == 0);
    check("levenshtein early exit reports 'too far'",
          levenshteinDistance("abcdef", "uvwxyz", 2) > 2);

    vector<string> fixed1, fixed2, fixed3, fixed4, fixed5;
    bool printedHint = false;

    {
        OutputSilencer quiet;

        fixed1 = applyAutocorrection(vector<string>(1, "pastaa"));

        printedHint = quiet.sink.str().find("Did you mean") != string::npos;

        fixed2 = applyAutocorrection(vector<string>(1, "serch"));
        fixed3 = applyAutocorrection(vector<string>(1, "xyzzyqq"));
        fixed4 = applyAutocorrection(vector<string>(1, "pasta"));
        fixed5 = applyAutocorrection(vector<string>(1, "mp3x"));
    }

    check("autocorrect 'pastaa' -> 'pasta'", fixed1[0] == "pasta");
    check("autocorrect 'serch' -> 'search'", fixed2[0] == "search");
    check("autocorrect leaves hopeless words alone", fixed3[0] == "xyzzyqq");
    check("autocorrect leaves known words alone", fixed4[0] == "pasta");
    check("autocorrect skips words containing digits", fixed5[0] == "mp3x");
    check("autocorrect prints a 'Did you mean' hint", printedHint);

    cout << "\n--- Boolean search ---\n";

    vector<int> andResult, orResult, notResult, emptyResult, badResult, mixedCase;

    {
        OutputSilencer quiet;   // malformed queries print an error message

        andResult = sortedIds(booleanSearch("pasta AND tomatoes"));
        orResult = sortedIds(booleanSearch("index OR pasta"));
        notResult = sortedIds(booleanSearch("index NOT trie"));
        emptyResult = sortedIds(booleanSearch("pasta AND index"));
        badResult = sortedIds(booleanSearch("pasta AND"));
        mixedCase = sortedIds(booleanSearch("Pasta and Tomatoes"));
    }

    check("Boolean AND: 'pasta AND tomatoes' -> doc 3", andResult == makeIds(2));
    check("Boolean OR: 'index OR pasta' -> docs 2, 3, 4", orResult == makeIds(1, 2, 3));
    check("Boolean NOT: 'index NOT trie' -> doc 2", notResult == makeIds(1));
    check("Boolean AND with no overlap -> empty", emptyResult.empty());
    check("Boolean malformed query -> empty", badResult.empty());
    check("Boolean operators are case-insensitive", mixedCase == makeIds(2));

    cout << "\n--- Phrase search & highlighting ---\n";

    check("phrase 'machine learning' found in doc 1",
          containsNormalizedPhrase(normalizedDocuments[0],
                                   normalizePhrase("machine learning")));
    check("phrase with wrong word order is not found",
          !containsNormalizedPhrase(normalizedDocuments[0],
                                    normalizePhrase("learning machine")));
    check("phrase matching respects word boundaries ('mach')",
          !containsNormalizedPhrase(normalizedDocuments[0],
                                    normalizePhrase("mach")));
    check("phrase matching ignores case and punctuation",
          containsNormalizedPhrase(normalizedDocuments[2],
                                   normalizePhrase("PASTA, sauce")));

    {
        vector<string> words(1, "learning");

        check("highlight wraps whole-word matches in **",
              highlightText("Machine learning", words) == "Machine **learning**");
        check("highlight keeps original capitalization",
              highlightText("LEARNING rocks", words) == "**LEARNING** rocks");
        check("highlight ignores partial-word matches",
              highlightText("unlearning", words) == "unlearning");
    }

    cout << "\n--- Autocomplete (Trie + history) ---\n";

    {
        vector<string> t = autocompleteTrie.autocomplete("ma", 10);
        bool hasMachine = find(t.begin(), t.end(), "machine") != t.end();

        check("Trie autocomplete 'ma' includes 'machine'", hasMachine);
    }

    {
        vector<Suggestion> s = getCombinedSuggestions("pa", 5);

        check("combined autocomplete falls back to the Trie ('pa' -> 'pasta')",
              containsSuggestion(s, "pasta"));
    }

    check("combined autocomplete with empty input -> nothing",
          getCombinedSuggestions("   ", 5).empty());

    cout << "\n--- Persistent history ---\n";

    {
        historyFile = "selftest_history.tmp";
        remove(historyFile.c_str());
        historyEnabled = true;

        recordQuery("Foo bar");
        recordQuery("foo  bar");
        recordQuery("baz");

        // Simulate a restart: wipe memory, reload from disk.
        searchHistory.clear();
        queryCounts.clear();

        int loaded = loadHistory();

        check("history file reloaded: 3 queries", loaded == 3 && searchHistory.size() == 3);
        check("query frequencies rebuilt ('foo bar' = 2)",
              queryCounts["foo bar"] == 2 && queryCounts["baz"] == 1);

        vector<Suggestion> s = getCombinedSuggestions("foo", 5);

        check("autocomplete suggests past query 'foo bar' from history",
              !s.empty() && s[0].text == "foo bar" && s[0].fromHistory &&
              s[0].score == 2);

        vector<Suggestion> s2 = getCombinedSuggestions("foo ", 5);

        check("trailing space completes from history only",
              containsSuggestion(s2, "foo bar"));

        clearHistory();

        check("clearHistory empties memory", searchHistory.empty() && queryCounts.empty());
        check("clearHistory empties the file", loadHistory() == 0);

        historyEnabled = false;
        remove(historyFile.c_str());
    }

    cout << "\n--- Result cache ---\n";

    {
        cacheEnabled = true;
        resultCache.clear();

        long before = cacheHits;

        RankedList first = runTFIDF(tokenize("pasta"), 5);
        RankedList second = runTFIDF(tokenize("pasta"), 5);

        check("second identical query is a cache hit", cacheHits == before + 1);
        check("cached results equal fresh results",
              idsOf(first) == idsOf(second));

        RankedList reordered = runTFIDF(tokenize("tomatoes pasta"), 5);
        long mid = cacheHits;
        RankedList swapped = runTFIDF(tokenize("pasta tomatoes"), 5);

        check("term order does not matter for the cache key",
              cacheHits == mid + 1 && idsOf(reordered) == idsOf(swapped));
    }

    cout << "\n====================================\n";
    cout << "Passed: " << testsPassed << "   Failed: " << testsFailed << "\n";
    cout << (testsFailed == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED") << "\n";
    cout << "====================================\n";

    return testsFailed == 0 ? 0 : 1;
}


// =====================================================
// MAIN FUNCTION
// =====================================================

int main(int argc, char** argv) {

    if (argc > 1 && string(argv[1]) == "--selftest") {
        return runSelfTests();
    }

    if (argc > 1) {
        dataFolder = argv[1];
    }

    if (argc > 2) {
        historyFile = argv[2];
    }

    cout << "====================================\n";
    cout << "       ADVANCED SEARCH ENGINE       \n";
    cout << "====================================\n";

    // Phase 6: Automatically load every .txt file from the Data folder.
    loadAllDocuments(dataFolder);

    if (documents.empty()) {

        cout << "\nNo documents were loaded.\n";

        cout << "Check your Data folder and file paths.\n";

        return 1;
    }

    // Build every search component (tokenizes each document once).
    buildEngine();

    cout << fixed << setprecision(2)
         << "Index built in " << indexBuildMs << " ms.\n";

    // Phase 8: restore previous searches, then start saving new ones.
    int restored = loadHistory();
    historyEnabled = true;

    cout << "Restored " << restored << " previous search"
         << (restored == 1 ? "" : "es") << " from " << historyFile << ".\n";

    int choice = 0;

    do {

        cout << "\n\n========== MAIN MENU ==========\n";

        cout << " 1. Search Documents (TF-IDF)\n";
        cout << " 2. Display Inverted Index\n";
        cout << " 3. Autocomplete (vocabulary + past searches)\n";
        cout << " 4. Advanced Search (Boolean / \"phrase\")\n";
        cout << " 5. Search History\n";
        cout << " 6. BM25 Search (Title / Content / Both)\n";
        cout << " 7. Compare TF-IDF vs BM25\n";
        cout << " 8. Query Suggestions (from past searches)\n";
        cout << " 9. Clear Search History\n";
        cout << "10. System Diagnostics\n";
        cout << "11. Performance Benchmark\n";
        cout << "12. Exit\n";

        cout << "\nEnter your choice: ";

        if (!(cin >> choice)) {

            if (cin.eof()) {
                break;
            }

            cin.clear();
            cin.ignore(10000, '\n');

            cout << "Invalid input. Enter a number.\n";

            choice = 0;

            continue;
        }

        cin.ignore(10000, '\n');

        switch (choice) {

            case 1: {

                string query;

                cout << "\nEnter search query: ";

                getline(cin, query);

                searchDocuments(query);

                break;
            }

            case 2: {

                displayInvertedIndex();

                break;
            }

            case 3: {

                string query;

                cout << "\nEnter prefix: ";

                getline(cin, query);

                autocompleteQuery(query, 5);

                break;
            }

            case 4: {

                string query;

                cout << "\nEnter advanced search query: ";

                getline(cin, query);

                advancedSearch(query);

                break;
            }

            case 5: {

                displaySearchHistory();
                break;
            }

            case 6: {

                string query;
                string fieldChoice;

                cout << "\nEnter search query: ";
                getline(cin, query);

                cout << "Search in: 1) Title  2) Content  3) Both [3]: ";
                getline(cin, fieldChoice);

                Field field = FIELD_BOTH;

                if (trimString(fieldChoice) == "1") {
                    field = FIELD_TITLE;
                }
                else if (trimString(fieldChoice) == "2") {
                    field = FIELD_CONTENT;
                }

                searchBM25(query, field);

                break;
            }

            case 7: {

                string query;

                cout << "\nEnter search query: ";

                getline(cin, query);

                compareRankings(query);

                break;
            }

            case 8: {

                string prefix;

                cout << "\nEnter query prefix (blank for top queries): ";

                getline(cin, prefix);

                suggestQueries(prefix, 5);

                break;
            }

            case 9: {

                string answer;

                cout << "\nDelete all saved search history? (y/n): ";

                getline(cin, answer);

                if (toLowerString(trimString(answer)) == "y") {

                    clearHistory();

                    cout << "Search history cleared.\n";
                }
                else {
                    cout << "Cancelled.\n";
                }

                break;
            }

            case 10: {

                displayDiagnostics();

                break;
            }

            case 11: {

                runBenchmark(1000);

                break;
            }

            case 12: {

                cout << "\nThank you for using "
                     << "Advanced Search Engine!\n";
                break;
            }

            default: {

                cout << "\nInvalid choice. Try again.\n";

            }
        }

    } while (choice != 12);

    return 0;

}