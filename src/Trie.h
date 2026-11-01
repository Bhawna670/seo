
#ifndef TRIE_H
#define TRIE_H

#include <string>
#include <vector>
#include <utility>

using namespace std;

class Trie {

private:

    struct TrieNode {

        TrieNode* children[26];

        bool isEndOfWord;

        int frequency;

        TrieNode();
    };

    TrieNode* root;

    void collectWords(
        TrieNode* node,
        const string& currentWord,
        vector<pair<string, int>>& suggestions
    );

    void deleteTrie(TrieNode* node);

    TrieNode* findPrefixNode(
        const string& prefix
    );

public:

    Trie();

    ~Trie();

    // Insert a word into the Trie
    void insert(
        const string& word,
        int frequency = 1
    );

    // Check whether a complete word exists
    bool search(
        const string& word
    );

    // Return autocomplete suggestions
    vector<string> autocomplete(
        const string& prefix,
        int limit = 5
    );
};

#endif
