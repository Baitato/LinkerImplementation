#include <iostream>
#include <fstream>
#include <vector>
#include <map>
#include <string>
#include <cstdint>
#include <cstring>

#include <elf.h>

struct SymbolEntry
{
    uint64_t value;
};

std::map<std::string, SymbolEntry> symbolTable;
std::vector<char> sectionNames;

void parseObjectFile(const std::string &fileName)
{
    std::ifstream inputFile(fileName, std::ios::binary);

    if (!inputFile)
    {
        std::cerr << "Failed to open input file: " << fileName << std::endl;
        return;
    }

    // Read the ELF header
    Elf64_Ehdr elfHeader;
    inputFile.read(reinterpret_cast<char *>(&elfHeader), sizeof(Elf64_Ehdr));

    // Read the section headers
    std::vector<Elf64_Shdr> sectionHeaders(elfHeader.e_shnum);
    inputFile.seekg(elfHeader.e_shoff);
    inputFile.read(reinterpret_cast<char *>(sectionHeaders.data()), elfHeader.e_shnum * sizeof(Elf64_Shdr));

    // Read the section name table
    Elf64_Shdr sectionNameTable = sectionHeaders[elfHeader.e_shstrndx];
    sectionNames.resize(sectionNameTable.sh_size);
    inputFile.seekg(sectionNameTable.sh_offset);
    inputFile.read(sectionNames.data(), sectionNameTable.sh_size);

    for (const auto &sectionHeader : sectionHeaders)
    {
        if (sectionHeader.sh_type == SHT_SYMTAB)
        {
            std::vector<Elf64_Sym> symbolEntries(sectionHeader.sh_size / sizeof(Elf64_Sym));
            inputFile.seekg(sectionHeader.sh_offset);
            inputFile.read(reinterpret_cast<char *>(symbolEntries.data()), sectionHeader.sh_size);

            for (const auto &symbolEntry : symbolEntries)
            {
                std::string symbolName = &sectionNames[sectionHeader.sh_name] + symbolEntry.st_name;
                SymbolEntry entry;
                entry.value = symbolEntry.st_value;
                symbolTable[symbolName] = entry;
            }
        }
    }

    inputFile.close();
}

void resolveSymbolReferences()
{
    for (auto &symbolEntry : symbolTable)
    {
        if (symbolEntry.second.value == 0)
        {
            const std::string &symbolName = symbolEntry.first;
            auto it = symbolTable.find(symbolName);
            if (it != symbolTable.end())
            {
                symbolEntry.second.value = it->second.value;
            }
        }
    }
}

void performRelocation(const std::string &fileName)
{
    std::fstream file(fileName, std::ios::in | std::ios::out | std::ios::binary);

    if (!file)
    {
        std::cerr << "Failed to open input file: " << fileName << std::endl;
        return;
    }

    // Read the ELF header
    Elf64_Ehdr elfHeader;
    file.read(reinterpret_cast<char *>(&elfHeader), sizeof(Elf64_Ehdr));

    // Read the section headers
    std::vector<Elf64_Shdr> sectionHeaders(elfHeader.e_shnum);
    file.seekg(elfHeader.e_shoff);
    file.read(reinterpret_cast<char *>(sectionHeaders.data()), elfHeader.e_shnum * sizeof(Elf64_Shdr));

    // Perform relocation
    for (auto &sectionHeader : sectionHeaders)
    {
        if (sectionHeader.sh_type == SHT_RELA)
        {
            size_t numRelocations = sectionHeader.sh_size / sizeof(Elf64_Rela);

            // Read relocation entries
            std::vector<Elf64_Rela> relocationEntries(numRelocations);
            file.seekg(sectionHeader.sh_offset);
            file.read(reinterpret_cast<char *>(relocationEntries.data()), sectionHeader.sh_size);

            // Apply relocations
            for (auto &relocationEntry : relocationEntries)
            {
                uint64_t *address = reinterpret_cast<uint64_t *>(elfHeader.e_type == ET_DYN ? relocationEntry.r_offset + sectionHeader.sh_addr : relocationEntry.r_offset);
                uint64_t symbolValue = symbolTable[sectionNames[relocationEntry.r_info >> 32] + (relocationEntry.r_info & 0xFFFFFFFF)].value;

                switch (ELF64_R_TYPE(relocationEntry.r_info))
                {
                case R_X86_64_64:
                    *address = symbolValue + relocationEntry.r_addend;
                    break;
                default:
                    std::cerr << "Unsupported relocation type." << std::endl;
                    break;
                }
            }
        }
    }

    file.close();
}

void writeExecutable(const std::string &outputFile)
{
    std::ifstream inputFile(outputFile, std::ios::binary);
    std::ofstream outputFileStream("a.out", std::ios::binary);

    outputFileStream << inputFile.rdbuf();

    inputFile.close();
    outputFileStream.close();
}

int main()
{
    std::string inputObjectFile = "input.o";

    // Parse the object file and build the symbol table
    parseObjectFile(inputObjectFile);

    // Resolve symbol references
    resolveSymbolReferences();

    // Perform relocations
    performRelocation(inputObjectFile);

    // Write the executable
    writeExecutable(inputObjectFile);

    std::cout << "Executable file generated: a.out" << std::endl;

    return 0;
}
