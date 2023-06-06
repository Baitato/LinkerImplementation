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

            // Read the entire file into a vector
            std::ifstream::pos_type fileSize = file.tellg();
            std::vector<char> fileData(fileSize);
            file.seekg(0, std::ios::beg);
            file.read(fileData.data(), fileSize);

            // Read each relocation entry and perform the relocation
            for (size_t i = 0; i < numRelocations; ++i)
            {
                Elf64_Rela relocationEntry;
                std::memcpy(&relocationEntry, &fileData[sectionHeader.sh_offset + i * sizeof(Elf64_Rela)], sizeof(Elf64_Rela));

                Elf64_Sym symbolEntry;
                std::memcpy(&symbolEntry, &fileData[sectionHeaders[sectionHeader.sh_link].sh_offset + ELF64_R_SYM(relocationEntry.r_info) * sizeof(Elf64_Sym)], sizeof(Elf64_Sym));

                // Check if the symbol is defined locally
                if (ELF64_ST_BIND(symbolEntry.st_info) == STB_LOCAL)
                {
                    continue;
                }

                std::string symbolName = &sectionNames[sectionHeaders[sectionHeader.sh_link].sh_name] + symbolEntry.st_name;

                // Check if the symbol is defined in the symbol table
                if (symbolTable.find(symbolName) == symbolTable.end())
                {
                    std::cerr << "Symbol not found: " << symbolName << std::endl;
                    continue;
                }

                SymbolEntry entry = symbolTable[symbolName];

                uint64_t symbolValue = entry.value;
                uint64_t relocationOffset = relocationEntry.r_offset;
                uint64_t addend = relocationEntry.r_addend;
                uint64_t type = ELF64_R_TYPE(relocationEntry.r_info);

                // Perform the relocation
                uint64_t *relocationAddress = reinterpret_cast<uint64_t *>(&fileData[relocationOffset]);
                uint64_t value = *relocationAddress;

                switch (type)
                {
                case R_X86_64_NONE:
                    break;
                case R_X86_64_64:
                    *relocationAddress = (value + symbolValue + addend);
                    break;
                case R_X86_64_PC32:
                    *relocationAddress = (value + symbolValue + addend - relocationOffset);
                    break;
                default:
                    std::cerr << "Unsupported relocation type: " << type << std::endl;
                    break;
                }
            }

            // Write the modified file data back to the file
            file.seekp(0, std::ios::beg);
            file.write(fileData.data(), fileSize);
        }
    }
}

void combineSections(const std::vector<std::string> &objectFiles, const std::string &outputFileName)
{
    std::ofstream outputFile(outputFileName, std::ios::binary);

    if (!outputFile)
    {
        std::cerr << "Failed to create output file: " << outputFileName << std::endl;
        return;
    }

    // Read the ELF header of the first object file
    std::ifstream firstObjectFile(objectFiles[0], std::ios::binary);
    Elf64_Ehdr firstElfHeader;
    firstObjectFile.read(reinterpret_cast<char *>(&firstElfHeader), sizeof(Elf64_Ehdr));
    firstObjectFile.close();

    // Write the ELF header to the output file
    outputFile.write(reinterpret_cast<char *>(&firstElfHeader), sizeof(Elf64_Ehdr));

    // Keep track of section offsets
    std::map<std::string, uint64_t> sectionOffsets;

    // Read the section name table of the first object file
    Elf64_Shdr sectionNameTable;
    firstObjectFile.open(objectFiles[0], std::ios::binary);
    firstObjectFile.seekg(firstElfHeader.e_shoff + firstElfHeader.e_shstrndx * sizeof(Elf64_Shdr));
    firstObjectFile.read(reinterpret_cast<char *>(&sectionNameTable), sizeof(Elf64_Shdr));

    std::vector<char> sectionNames(sectionNameTable.sh_size);
    firstObjectFile.seekg(sectionNameTable.sh_offset);
    firstObjectFile.read(sectionNames.data(), sectionNameTable.sh_size);
    firstObjectFile.close();

    // Write the section name table to the output file
    outputFile.seekp(sectionNameTable.sh_offset);
    outputFile.write(sectionNames.data(), sectionNameTable.sh_size);

    // Iterate over the section headers of the first object file
    std::ifstream inputFile(objectFiles[0], std::ios::binary);
    inputFile.seekg(firstElfHeader.e_shoff);
    std::vector<Elf64_Shdr> sectionHeaders(firstElfHeader.e_shnum);
    inputFile.read(reinterpret_cast<char *>(sectionHeaders.data()), firstElfHeader.e_shnum * sizeof(Elf64_Shdr));

    // Write the section headers to the output file
    outputFile.seekp(firstElfHeader.e_shoff);
    outputFile.write(reinterpret_cast<char *>(sectionHeaders.data()), firstElfHeader.e_shnum * sizeof(Elf64_Shdr));

    // Calculate section offsets
    for (const auto &sectionHeader : sectionHeaders)
    {
        if (sectionHeader.sh_type != SHT_NULL && sectionHeader.sh_type != SHT_NOBITS)
        {
            sectionOffsets[&sectionNames[sectionHeader.sh_name]] = sectionHeader.sh_offset;
        }
    }

    // Iterate over the object files to combine sections
    for (const auto &objectFile : objectFiles)
    {
        if (objectFile == objectFiles[0])
        {
            continue; // Skip the first object file, as its sections are already written
        }

        // Read the ELF header
        Elf64_Ehdr elfHeader;
        inputFile.open(objectFile, std::ios::binary);
        inputFile.read(reinterpret_cast<char *>(&elfHeader), sizeof(Elf64_Ehdr));

        // Read the section headers
        sectionHeaders.resize(elfHeader.e_shnum);
        inputFile.seekg(elfHeader.e_shoff);
        inputFile.read(reinterpret_cast<char *>(sectionHeaders.data()), elfHeader.e_shnum * sizeof(Elf64_Shdr));

        // Write the sections to the output file
        for (size_t sectionIndex = 0; sectionIndex < sectionHeaders.size(); sectionIndex++)
        {
            const auto &sectionHeader = sectionHeaders[sectionIndex];

            if (sectionHeader.sh_type != SHT_NULL && sectionHeader.sh_type != SHT_NOBITS)
            {
                // Calculate the new section offset based on the current section offset and the previous section size
                uint64_t previousSectionSize = sectionOffsets[&sectionNames[sectionHeader.sh_name]];
                uint64_t newSectionOffset = sectionOffsets[&sectionNames[sectionHeader.sh_name]] + previousSectionSize;

                // Update the section offset in the section header
                sectionOffsets[&sectionNames[sectionHeader.sh_name]] = newSectionOffset;
                sectionHeaders[sectionIndex].sh_offset = newSectionOffset;

                // Write the section header to the output file
                outputFile.seekp(sectionHeader.sh_offset);
                outputFile.write(reinterpret_cast<char *>(&sectionHeaders[sectionIndex]), sizeof(Elf64_Shdr));

                // Read the section data from the input file
                std::vector<char> sectionData(sectionHeader.sh_size);
                inputFile.seekg(sectionHeader.sh_offset);
                inputFile.read(sectionData.data(), sectionHeader.sh_size);

                // Write the section data to the output file
                outputFile.seekp(sectionHeader.sh_offset);
                outputFile.write(sectionData.data(), sectionHeader.sh_size);
            }
        }

        inputFile.close();
    }

    outputFile.close();
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        std::cerr << "Usage: " << argv[0] << " <output_file> <input_file1> [<input_file2> ...]" << std::endl;
        return 1;
    }

    std::string outputFileName = argv[1];
    std::vector<std::string> objectFiles(argv + 2, argv + argc);

    // Combine sections from object files
    combineSections(objectFiles, outputFileName);

    // Parse each object file to build the symbol table
    for (const auto &objectFile : objectFiles)
    {
        parseObjectFile(objectFile);
    }

    // Resolve symbol references
    resolveSymbolReferences();

    // Perform relocations
    for (const auto &objectFile : objectFiles)
        performRelocation(objectFile);

    std::cout << "Combined object files and performed relocations." << std::endl;

    return 0;
}

// Command to run : ./linker output.o addition.o subtraction.o