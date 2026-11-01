
#include "Trie.h"

#include <algorithm>
#include <iostream>

using namespace std;

// --------------------------------------
// TrieNode Constructor
// --------------------------------------

Trie::TrieNode::TrieNode() {

    for (int i = 0; i < 26; i++) {

        children[i] = nullptr;
    }

    isEndOfWord = false;

    frequency = 0;
}

// --------------------------------------
// Trie Constructor
// --------------------------------------

Trie::Trie() {

    root = new TrieNode();
}

// --------------------------------------
// Insert Word
// --------------------------------------

void Trie::insert(
    const string& word,
    int frequency
) {

    TrieNode* current = root;

    for (char c : word) {

        // Support lowercase English letters only

        if (c < 'a' || c > 'z') {
            return;
        }

        int index = c - 'a';

        if (current->children[index] == nullptr) {

            current->children[index] =
                new TrieNode();
        }

        current = current->children[index];
    }

    current->isEndOfWord = true;

    current->frequency += frequency;
}

// --------------------------------------
// Search Complete Word
// --------------------------------------

bool Trie::search(
    const string& word
) {

    TrieNode* current = root;

    for (char c : word) {

        if (c < 'a' || c > 'z') {
            return false;
        }

        int index = c - 'a';

        if (current->children[index] == nullptr) {
            return false;
        }

        current = current->children[index];
    }

    return current->isEndOfWord;
}

// --------------------------------------
// Find Prefix Node
// --------------------------------------

Trie::TrieNode* Trie::findPrefixNode(
    const string& prefix
) {

    TrieNode* current = root;

    for (char c : prefix) {

        if (c < 'a' || c > 'z') {
            return nullptr;
        }

        int index = c - 'a';

        if (current->children[index] == nullptr) {
            return nullptr;
        }

        current = current->children[index];
    }

    return current;
}

// --------------------------------------
// Collect Words Using DFS
// --------------------------------------

void Trie::collectWords(
    TrieNode* node,
    const string& currentWord,
    vector<pair<string, int>>& suggestions
) {

    if (node == nullptr) {
        return;
    }

    // Store the word if it ends here

    if (node->isEndOfWord) {

        suggestions.push_back({
            currentWord,
            node->frequency
        });
    }

    // Explore child nodes

    for (int i = 0; i < 26; i++) {

        if (node->children[i] != nullptr) {

            char nextChar = 'a' + i;

            collectWords(
                node->children[i],
                currentWord + nextChar,
                suggestions
            );
        }
    }
}

// --------------------------------------
// Autocomplete
// --------------------------------------

vector<string> Trie::autocomplete(
    const string& prefix,
    int limit
) {

    if (limit <= 0) {
        return {};
    }

    vector<pair<string, int>> suggestions;

    TrieNode* prefixNode =
        findPrefixNode(prefix);

    if (prefixNode == nullptr) {
        return {};
    }

    collectWords(
        prefixNode,
        prefix,
        suggestions
    );

    // Sort by frequency descending
    // Alphabetically when frequencies are equal

    sort(
        suggestions.begin(),
        suggestions.end(),
        [](const pair<string, int>& a,
           const pair<string, int>& b) {

            if (a.second != b.second) {

                return a.second > b.second;
            }

            return a.first < b.first;
        }
    );

    vector<string> results;

    int count = min(
        limit,
        static_cast<int>(suggestions.size())
    );

    for (int i = 0; i < count; i++) {

        results.push_back(
            suggestions[i].first
        );
    }

    return results;
}

// --------------------------------------
// Delete Trie Nodes
// --------------------------------------

void Trie::deleteTrie(
    TrieNode* node
) {

    if (node == nullptr) {
        return;
    }

    for (int i = 0; i < 26; i++) {

        deleteTrie(node->children[i]);
    }

    delete node;
}

// --------------------------------------
// Trie Destructor
// --------------------------------------

Trie::~Trie() {

    deleteTrie(root);
}
