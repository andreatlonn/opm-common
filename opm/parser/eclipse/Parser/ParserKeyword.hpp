/*
  Copyright 2013 Statoil ASA.

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef PARSER_KEYWORD_H
#define PARSER_KEYWORD_H

#include <iosfwd>
#include <string>
#include <memory>
#include <set>

#include <boost/regex.hpp>

#include <opm/parser/eclipse/Parser/ParserEnums.hpp>
#include <opm/parser/eclipse/Parser/ParserRecord.hpp>

namespace Json {
    class JsonObject;
}

namespace Opm {
    class Deck;
    class DeckKeyword;
    class ParseContext;
    class ParserDoubleItem;
    class RawKeyword;
    class string_view;
    class MessageContainer;

    class ParserKeyword {
    public:
        ParserKeyword(const std::string& name ,
                      const std::string& sizeKeyword ,
                      const std::string& sizeItem,
                      bool _isTableCollection = false);
        explicit ParserKeyword(const std::string& name);
        explicit ParserKeyword(const Json::JsonObject& jsonConfig);

        void setFixedSize( size_t keywordSize);
        void setSizeType( ParserKeywordSizeEnum sizeType );
        void setTableCollection(bool _isTableCollection);
        void initSizeKeyword( const std::string& sizeKeyword, const std::string& sizeItem);


        typedef std::set<std::string> DeckNameSet;
        typedef std::set<std::string> SectionNameSet;


        static string_view getDeckName(const string_view& rawString);
        static bool validInternalName(const std::string& name);
        static bool validDeckName(const string_view& name);
        bool hasMatchRegex() const;
        void setMatchRegex(const std::string& deckNameRegexp);
        bool matches(const string_view& ) const;
        bool hasDimension() const;
        void addRecord( ParserRecord );
        void addDataRecord( ParserRecord );
        const ParserRecord& getRecord(size_t recordIndex) const;
        ParserRecord& getRecord(size_t recordIndex);
        std::vector< ParserRecord >::const_iterator begin() const;
        std::vector< ParserRecord >::const_iterator end() const;
        const std::string className() const;
        const std::string& getName() const;
        size_t getFixedSize() const;
        bool hasFixedSize() const;
        bool isTableCollection() const;
        std::string getDescription() const;
        void setDescription(const std::string &description);

        bool hasMultipleDeckNames() const;
        void clearDeckNames();
        void addDeckName( const std::string& deckName );
        DeckNameSet::const_iterator deckNamesBegin() const;
        DeckNameSet::const_iterator deckNamesEnd() const;

        void clearValidSectionNames();
        void addValidSectionName(const std::string& sectionName);
        bool isValidSection(const std::string& sectionName) const;
        SectionNameSet::const_iterator validSectionNamesBegin() const;
        SectionNameSet::const_iterator validSectionNamesEnd() const;

        DeckKeyword parse(const ParseContext& parseContext , MessageContainer& msgContainer, std::shared_ptr< RawKeyword > rawKeyword) const;
        enum ParserKeywordSizeEnum getSizeType() const;
        const std::pair<std::string,std::string>& getSizeDefinitionPair() const;
        bool isDataKeyword() const;

        std::string createDeclaration(const std::string& indent) const;
        std::string createDecl() const;
        std::string createCode() const;
        void applyUnitsToDeck( Deck& deck, DeckKeyword& deckKeyword) const;

        bool operator==( const ParserKeyword& ) const;
        bool operator!=( const ParserKeyword& ) const;

    private:
        std::pair<std::string,std::string> m_sizeDefinitionPair;
        std::string m_name;
        DeckNameSet m_deckNames;
        DeckNameSet m_validSectionNames;
        std::string m_matchRegexString;
        boost::regex m_matchRegex;
        std::vector< ParserRecord > m_records;
        enum ParserKeywordSizeEnum m_keywordSizeType;
        size_t m_fixedSize;
        bool m_isTableCollection;
        std::string m_Description;

        static bool validNameStart(const string_view& name);
        void initDeckNames( const Json::JsonObject& jsonConfig );
        void initSectionNames( const Json::JsonObject& jsonConfig );
        void initMatchRegex( const Json::JsonObject& jsonObject );
        void initData( const Json::JsonObject& jsonConfig );
        void initSize( const Json::JsonObject& jsonConfig );
        void initSizeKeyword(const Json::JsonObject& sizeObject);
        void commonInit(const std::string& name, ParserKeywordSizeEnum sizeType);
        void addItems( const Json::JsonObject& jsonConfig);
    };

std::ostream& operator<<( std::ostream&, const ParserKeyword& );
}

#endif
