Advanced Search Engine: Ranking & Autocomplete
Project Overview
The project is a C++-based search engine that searches text documents, retrieves relevant results, and ranks them using information-retrieval algorithms. It also provides autocomplete, spelling correction, Boolean search, and query suggestions.

Project Location: E:\seo
Language: C++
Compiler: MinGW GCC 6.3.0
Document Folder: E:\seo\Data
Work Completed (Phases 1–7)
1. Document Reading & Preprocessing: Reads text files, converts text to lowercase, removes punctuation, and tokenizes words.
2. Inverted Index: Stores terms with their document IDs, term frequencies, and document frequencies.
3. Trie-Based Autocomplete: Provides word suggestions based on user input.
4. TF-IDF Ranking: Calculates TF-IDF scores and uses cosine similarity to rank relevant documents.
5. Advanced Search: Implements Boolean search (AND, OR, NOT), exact phrase search, and result highlighting.
6. Performance Improvements: Automatically loads .txt files from the Data folder, retrieves candidate documents through the inverted index, displays the top five results, maintains search history, and measures execution time.
7. Advanced Search Features:
   - BM25 ranking.
   - Query autocorrection using Levenshtein distance.
   - Search suggestions based on previous queries and their frequency.
   - Field-based search (title, content, or both).
   - Comparison of TF-IDF and BM25 rankings.
Note: These features have been implemented in the code, but complete testing with the local compiler and actual documents is still pending.
Work Remaining
Phase 8 – Persistence & Optimization
- Save search history permanently in a file.
- Load previous search history when the application starts.
- Improve autocomplete by combining Trie suggestions with previous searches.
- Optimize query processing and result retrieval.
Phase 9 – Testing & Finalization
- Test every search feature with different queries.
- Verify TF-IDF and BM25 ranking accuracy.
- Test Boolean, phrase, autocomplete, and autocorrection functionality.
- Check performance and resolve bugs.
- Add system diagnostics.
- Prepare project documentation and README.
- Capture screenshots and prepare the project for GitHub/resume presentation.
Current Project Structure
E:\seo\
│
├── src\
│   ├── main.cpp
│   ├── Trie.h
│   └── Trie.cpp
│
├── Data\
│   ├── doc1.txt
│   ├── doc2.txt
│   └── doc3.txt
│
└── search_engine.exe


Important Technical Notes
- The project uses the Windows API for automatic file loading instead of std::filesystem, because the installed MinGW compiler is older.
- The existing Trie implementation must be preserved. Its interface uses insert(word, frequency) and autocomplete(prefix, limit).
- There is a TF-IDF term-frequency increment issue to check and correct.
- The application should be compiled from the project root using:
cd E:\seo
g++ -std=c++17 src/main.cpp src/Trie.cpp -o search_engine.exe
.\search_engine.exe


Final Goal
Complete a fully functional Advanced Search Engine that supports efficient document retrieval, intelligent ranking, autocomplete, spelling correction, query suggestions, and persistent search history.

Overall progress: Phases 1–7 implemented; Phases 8–9 remaining for completion, testing, optimization, and final documentation.