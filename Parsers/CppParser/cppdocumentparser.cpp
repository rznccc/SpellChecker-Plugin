﻿/**************************************************************************
**
** Copyright (c) 2014 Carel Combrink
**
** This file is part of the SpellChecker Plugin, a Qt Creator plugin.
**
** The SpellChecker Plugin is free software: you can redistribute it and/or
** modify it under the terms of the GNU Lesser General Public License as
** published by the Free Software Foundation, either version 3 of the
** License, or (at your option) any later version.
**
** The SpellChecker Plugin is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU Lesser General Public License for more details.
**
** You should have received a copy of the GNU Lesser General Public License
** along with the SpellChecker Plugin.  If not, see <http://www.gnu.org/licenses/>.
****************************************************************************/

#include "../../spellcheckercore.h"
#include "../../spellcheckercoresettings.h"
#include "../../spellcheckerconstants.h"
#include "../../Word.h"
#include "cppdocumentparser.h"
#include "cppparsersettings.h"
#include "cppparseroptionspage.h"
#include "cppparserconstants.h"
#include "cplusplusdocumentparser.h"

#include <coreplugin/icore.h>
#include <coreplugin/actionmanager/actioncontainer.h>
#include <coreplugin/actionmanager/actionmanager.h>
#include <cpptools/cppmodelmanager.h>
#include <cpptools/cpptoolsreuse.h>
#include <cppeditor/cppeditorconstants.h>
#include <cppeditor/cppeditordocument.h>
#include <texteditor/texteditor.h>
#include <texteditor/syntaxhighlighter.h>
#include <projectexplorer/project.h>
#include <projectexplorer/session.h>
#include <utils/algorithm.h>
#include <utils/mimetypes/mimedatabase.h>
#include <utils/runextensions.h>

#include <QRegularExpression>
#include <QTextBlock>
#include <QApplication>
#include <QFutureWatcher>

/*! \brief Testing assert that should be used during debugging
 * but should not be made part of a release. */
//#define SP_CHECK( test ) QTC_CHECK( test )
#define SP_CHECK( test )

namespace SpellChecker {
namespace CppSpellChecker {
namespace Internal {

/*! Mime Type for the C++ doxygen files.
 * This must match the 'mime-type type' in the json.in file of this
 * plugin. */
const char MIME_TYPE_CXX_DOX[] = "text/x-c++dox";

//--------------------------------------------------
//--------------------------------------------------
//--------------------------------------------------

#ifdef FUTURE_NOT_WORKING
using ParserMap     = QMap<CPlusPlusDocumentParser*, QString> ;
using ParserMapIter = ParserMap::Iterator;
#else
using FutureWatcherMap     = QMap<QFutureWatcher<CPlusPlusDocumentParser::ResultType>*, QString> ;
using FutureWatcherMapIter = FutureWatcherMap::Iterator;
#endif

class CppDocumentParserPrivate {
public:
    ProjectExplorer::Project* activeProject;
    QString currentEditorFileName;
    CppParserOptionsPage* optionsPage;
    CppParserSettings* settings;
    QStringSet filesInStartupProject;

    HashWords tokenHashes;
    QMutex futureMutex;
#ifdef FUTURE_NOT_WORKING
    ParserMap parserMap;
#else
    FutureWatcherMap futureWatchers;
#endif
    QStringList filesInProcess;

    CppDocumentParserPrivate() :
        activeProject(nullptr),
        currentEditorFileName(),
        filesInStartupProject()
    {}

    /*! \brief Get all C++ files from the \a list of files.
     *
     * This function uses the MIME Types of the passed files and check if
     * they are classified by the CppTools::ProjectFile class. If they are
     * they are regarded as C++ files.
     * If a file is unsupported the type is checked against the
     * custom MIME Type added by this plygin. */
    QStringSet getCppFiles(const QStringSet& list)
    {
        const QStringSet filteredList = Utils::filtered(list, [](const QString& file){
        const CppTools::ProjectFile::Kind kind = CppTools::ProjectFile::classify(file);
        switch(kind){
        case CppTools::ProjectFile::Unclassified:
          return false;
        case CppTools::ProjectFile::Unsupported: {
          /* Check our doxy MimeType added by this plugin */
          const Utils::MimeType mimeType = Utils::mimeTypeForFile(file);
          const QString mt = mimeType.name();
          if (mt == QLatin1String(MIME_TYPE_CXX_DOX)){
              return true;
          } else {
            return false;
          }
        }
        default:
            return true;
        }
      });

      return filteredList;
    }
    // ------------------------------------------
};
//--------------------------------------------------
//--------------------------------------------------
//--------------------------------------------------

CppDocumentParser::CppDocumentParser(QObject *parent) :
    IDocumentParser(parent),
    d(new CppDocumentParserPrivate())
{
    /* Create the settings for this parser */
    d->settings = new SpellChecker::CppSpellChecker::Internal::CppParserSettings();
    d->settings->loadFromSettings(Core::ICore::settings());
    connect(d->settings, &CppParserSettings::settingsChanged, this, &CppDocumentParser::settingsChanged);
    connect(SpellCheckerCore::instance()->settings(), &SpellChecker::Internal::SpellCheckerCoreSettings::settingsChanged, this, &CppDocumentParser::settingsChanged);
    /* Crete the options page for the parser */
    d->optionsPage = new CppParserOptionsPage(d->settings, this);

    CppTools::CppModelManager *modelManager = CppTools::CppModelManager::instance();
    connect(modelManager, &CppTools::CppModelManager::documentUpdated, this, &CppDocumentParser::parseCppDocumentOnUpdate, Qt::DirectConnection);
    connect(qApp, &QApplication::aboutToQuit, this, [=](){
      /* Disconnect any signals that might still get emitted. */
      modelManager->disconnect(this);
      SpellCheckerCore::instance()->disconnect(this);
      this->disconnect(SpellCheckerCore::instance());
    }, Qt::DirectConnection);

    Core::Context context(CppEditor::Constants::CPPEDITOR_ID);
    Core::ActionContainer *cppEditorContextMenu= Core::ActionManager::createMenu(CppEditor::Constants::M_CONTEXT);
    Core::ActionContainer *contextMenu = Core::ActionManager::createMenu(Constants::CONTEXT_MENU_ID);
    cppEditorContextMenu->addSeparator(context);
    cppEditorContextMenu->addMenu(contextMenu);
}
//--------------------------------------------------

CppDocumentParser::~CppDocumentParser()
{
    d->settings->saveToSetting(Core::ICore::settings());
    delete d->settings;
    delete d->optionsPage;
    delete d;
}
//--------------------------------------------------

QString CppDocumentParser::displayName()
{
    return tr("C++ Document Parser");
}
//--------------------------------------------------

Core::IOptionsPage *CppDocumentParser::optionsPage()
{
    return d->optionsPage;
}
//--------------------------------------------------

void CppDocumentParser::setActiveProject(ProjectExplorer::Project *activeProject)
{
    d->activeProject = activeProject;
    d->filesInStartupProject.clear();
    if(d->activeProject == nullptr) {
        return;
    }
    reparseProject();
}
//--------------------------------------------------

void CppDocumentParser::updateProjectFiles(QStringSet filesAdded, QStringSet filesRemoved)
{
  Q_UNUSED(filesRemoved)
  /* Only re-parse the files that were added. */
  CppTools::CppModelManager *modelManager = CppTools::CppModelManager::instance();

  const QStringSet fileSet = d->getCppFiles(filesAdded);
  d->filesInStartupProject.unite(fileSet);
  modelManager->updateSourceFiles(fileSet);
}
//--------------------------------------------------

void CppDocumentParser::setCurrentEditor(const QString& editorFilePath)
{
    d->currentEditorFileName = editorFilePath;
}
//--------------------------------------------------

void CppDocumentParser::parseCppDocumentOnUpdate(CPlusPlus::Document::Ptr docPtr)
{
    if(docPtr.isNull() == true) {
        return;
    }

    const QString fileName = docPtr->fileName();
    if(shouldParseDocument(fileName) == false) {
        return;
    }
    WordList words = parseCppDocument(std::move(docPtr));
//    /* Now that we have all of the words from the parser, emit the signal
//     * so that they will get spell checked. */
//    emit spellcheckWordsParsed(fileName, words);
}
//--------------------------------------------------

void CppDocumentParser::settingsChanged()
{
    /* Clear the hashes since all comments must be re parsed. */
    d->tokenHashes.clear();
    /* Re parse the project */
    reparseProject();
}
//--------------------------------------------------

void CppDocumentParser::reparseProject()
{
    d->filesInStartupProject.clear();
    if(d->activeProject == nullptr) {
        return;
    }
    CppTools::CppModelManager *modelManager = CppTools::CppModelManager::instance();

    const Utils::FileNameList projectFiles = d->activeProject->files(ProjectExplorer::Project::SourceFiles);
    const QStringList fileList = Utils::transform(projectFiles, &Utils::FileName::toString);

    const QStringSet fileSet = d->getCppFiles(fileList.toSet());
    d->filesInStartupProject = fileSet;
    modelManager->updateSourceFiles(fileSet);
}
//--------------------------------------------------

bool CppDocumentParser::shouldParseDocument(const QString& fileName)
{
    SpellChecker::Internal::SpellCheckerCoreSettings* settings = SpellCheckerCore::instance()->settings();
    if((settings->onlyParseCurrentFile == true)
            && (d->currentEditorFileName != fileName)) {
        /* The global setting is set to only parse the current file and the
         * file asked about is not the current one, thus do not parse it. */
        return false;
    }

    CppTools::CppModelManager *modelManager = CppTools::CppModelManager::instance();
    auto snap = modelManager->snapshot();

    if((settings->checkExternalFiles) == false) {
        /* Do not check external files so check if the file is part of the
         * active project. */
        return d->filesInStartupProject.contains(fileName);
    }

    return true;
}
//--------------------------------------------------

void CppDocumentParser::futureFinished()
{
    /* Get the watcher from the sender() of the signal that invoked this slot.
     * reinterpret_cast is used since qobject_cast is not valid of template
     * classes since the template class does not have the Q_OBJECT macro. */

#ifdef FUTURE_NOT_WORKING
    CPlusPlusDocumentParser* parser = qobject_cast<CPlusPlusDocumentParser*>(sender());
    if(parser == nullptr) {
      qDebug() << "INVALID WATCHER";
      return;
    }
    const CPlusPlusDocumentParser::ResultType result = parser->result();
#else
    QFutureWatcher<CPlusPlusDocumentParser::ResultType> *watcher = reinterpret_cast<QFutureWatcher<CPlusPlusDocumentParser::ResultType>*>(sender());
    if(watcher == nullptr) {
      qDebug() << "INVALID WATCHER";
      return;
    }
    if(watcher->isCanceled() == true) {
        /* Application is shutting down */
        return;
    }
    const CPlusPlusDocumentParser::ResultType result = watcher->result();
#endif

    qDebug() << "FUTURE DONE: " << this->thread();

    const QStringSet wordsInSource           = result.first;
    const QVector<WordTokens> tokenizedWords = result.second;


    QMutexLocker locker(&d->futureMutex);
    /* Get the file name associated with this future and the misspelled
     * words. */
#ifdef FUTURE_NOT_WORKING
    ParserMapIter iter = d->parserMap.find(parser);
    if(iter == d->parserMap.end()) {
      return;
    }
    QString fileName = iter.value();
    d->parserMap.erase(iter);
#else
    FutureWatcherMapIter iter = d->futureWatchers.find(watcher);
    if(iter == d->futureWatchers.end()) {
        return;
    }
    QString fileName = iter.value();
    d->futureWatchers.erase(iter);
#endif
    d->filesInProcess.removeAll(fileName);

    /* Make a local copy of the last list of hashes. A local copy is made and used
     * as the input the tokenize function, but a new list is returned from the
     * tokenize function. If this is not done the list of hashes can grow forever
     * and cause a huge increase in memory. Doing it this way ensure that the
     * list only contains hashes of tokens that are present in during the last run
     * and will not contain old and invalid hashes. It will cause the parsing of a
     * different file than the previous run to be less efficient but if a file is
     * parsed multiple times, one after the other, it will result in a large speed up
     * and this will mostly be the case when editing a file. For this reason the initial
     * project parse on start up can be slower. */

    /* Populate the list of hashes from the tokens that was processed. */
    HashWords newHashesOut;
    WordList newSettingsApplied;
    for(const WordTokens& token: qAsConst(tokenizedWords)) {
      WordList words = token.words;
      if(token.newHash == true) {
        /* The words are new, they were not known in a previous hash
         * thus the settings must now be applied.
         * Only words that have already been checked against the settings
         * gets added to the hash, thus there is no need to apply the settings
         * again, since this will only waste time. */
        applySettingsToWords(token.string, words, wordsInSource);
      }
      newSettingsApplied.append(words);
      SP_CHECK(token.hash != 0x00);
      newHashesOut[token.hash] = {token.line, token.column, words};
    }
    /* Move the new list of hashes to the member data so that
     * it can be used the next time around. Move is made explicit since
     * the LHS can be removed and the RHS will not be used again from
     * here on. */
    d->tokenHashes = std::move(newHashesOut);

    /* Now that we have all of the words from the parser, emit the signal
     * so that they will get spell checked. */
    emit spellcheckWordsParsed(fileName, newSettingsApplied);
}

WordList CppDocumentParser::parseCppDocument(CPlusPlus::Document::Ptr docPtr)
{
    const QString fileName = docPtr->fileName();
    qDebug() << fileName <<":" << this->thread();
    using ResultType = CPlusPlusDocumentParser::ResultType;
    CPlusPlusDocumentParser *parser = new CPlusPlusDocumentParser(docPtr, d->tokenHashes, *d->settings);
    docPtr.reset();

#ifdef FUTURE_NOT_WORKING
    connect(parser, &CPlusPlusDocumentParser::done, this, [](){ qDebug() << "FUTURE DONE"; }, Qt::QueuedConnection);
    connect(parser, &CPlusPlusDocumentParser::done, this, &CppDocumentParser::futureFinished, Qt::QueuedConnection);
    connect(parser, &CPlusPlusDocumentParser::done, parser, &CPlusPlusDocumentParser::deleteLater);
    d->parserMap.insert(parser, fileName);
#else
    QFutureWatcher<ResultType> *watcher = new QFutureWatcher<ResultType>();
    connect(watcher, &QFutureWatcher<ResultType>::finished, this, [](){ qDebug() << "FUTURE DONE"; }, Qt::QueuedConnection);
    connect(watcher, &QFutureWatcher<ResultType>::finished, this, &CppDocumentParser::futureFinished, Qt::QueuedConnection);
    connect(watcher, &QFutureWatcher<ResultType>::finished, parser, &CPlusPlusDocumentParser::deleteLater);
    d->futureWatchers.insert(watcher, fileName);
#endif

    d->filesInProcess.append(fileName);


    /* Run the processor in the background and set a watcher to monitor the progress. */
#ifdef FUTURE_NOT_WORKING
    QFuture<void> future = Utils::runAsync(QThreadPool::globalInstance(), QThread::HighestPriority, &CPlusPlusDocumentParser::process, parser);
#else
    QFuture<ResultType> future = Utils::runAsync(QThreadPool::globalInstance(), QThread::HighPriority, &CPlusPlusDocumentParser::process, parser);
    qDebug() << "FUTURE CREATED";
    watcher->setFuture(future);
    qDebug() << "WATCHERS SET";
#endif

//    watcher->waitForFinished();
//    qDebug() << "DONE";
//    bool success = parser.run();
//    SP_CHECK(success == true);


//    return newSettingsApplied;
    return {};
}
//--------------------------------------------------

void CppDocumentParser::applySettingsToWords(const QString &string, WordList &words, const QStringSet &wordsInSource)
{
    using namespace SpellChecker::Parsers::CppParser;

    /* Filter out words that appears in the source. They are checked against the list
     * of words parsed from the file before the for loop. */
    if(d->settings->removeWordsThatAppearInSource == true) {
        removeWordsThatAppearInSource(wordsInSource, words);
    }

    /* Regular Expressions that might be used, defined here so that it does not get cleared in the loop.
     * They are made static const because they will be re-used a lot and will never be changed. This way
     * the construction of the objects can be done once and then be re-used. */
    static const QRegularExpression doubleRe(QStringLiteral("\\A\\d+(\\.\\d+)?\\z"));
    static const QRegularExpression hexRe(QStringLiteral("\\A0x[0-9A-Fa-f]+\\z"));
    static const QRegularExpression emailRe(QStringLiteral("\\A") + QLatin1String(SpellChecker::Parsers::CppParser::Constants::EMAIL_ADDRESS_REGEXP_PATTERN) + QStringLiteral("\\z"));
    static const QRegularExpression websiteRe(QString() + QLatin1String(SpellChecker::Parsers::CppParser::Constants::WEBSITE_ADDRESS_REGEXP_PATTERN));
    static const QRegularExpression websiteCharsRe(QString() + QLatin1String(SpellChecker::Parsers::CppParser::Constants::WEBSITE_CHARS_REGEXP_PATTERN));
    /* Word list that can be added to in the case that a word is split up into different words
     * due to some setting or rule. These words can also be checked against the settings using
     * recursion or not. It depends on the implementation that did the splitting of the
     * original word. It is done in this way so that the iterator that is currently operating
     * on the list of words does not break when new words get added during iteration */
    WordList wordsToAddInTheEnd;
    /* Iterate through the list of words using an iterator and remove words according to settings */
    WordList::Iterator iter = words.begin();
    while(iter != words.end()) {
        QString currentWord = (*iter).text;
        QString currentWordCaps = currentWord.toUpper();
        bool removeCurrentWord = false;

        /* Remove reserved words first. Although this does not depend on settings, this
         * is done here to prevent multiple iterations through the word list where possible */
        removeCurrentWord = isReservedWord(currentWord);

        if(removeCurrentWord == false) {
            /* Remove the word if it is a number, checking for floats and doubles as well. */
            if(doubleRe.match(currentWord).hasMatch() == true) {
                removeCurrentWord = true;
            }
            /* Remove the word if it is a hex number. */
            if(hexRe.match(currentWord).hasMatch() == true) {
                removeCurrentWord = true;
            }
        }

        if((removeCurrentWord == false) && (d->settings->checkQtKeywords == false)) {
            /* Remove the basic Qt Keywords using the isQtKeyword() function in the CppTools */
            if((CppTools::isQtKeyword(QStringRef(&currentWord)) == true)
                    || (CppTools::isQtKeyword(QStringRef(&currentWordCaps)) == true)){
                removeCurrentWord = true;
            }
            /* Remove words that Start with capital Q and the next char is also capital letter. This would
             * only apply to words longer than 2 characters long. This check is also to ensure that we do
             * not go past the size of the word */
            if(currentWord.length() > 2) {
                if((currentWord.at(0) == QLatin1Char('Q')) && (currentWord.at(1).isUpper() == true)) {
                    removeCurrentWord = true;
                }
            }

            /* Remove all caps words that start with Q_ */
            if(currentWord.startsWith(QLatin1String("Q_"), Qt::CaseSensitive) == true) {
                removeCurrentWord = true;
            }

            /* Remove qDebug() */
            if(currentWord == QLatin1String("qDebug")) {
                removeCurrentWord = true;
            }
        }

        if((d->settings->removeEmailAddresses == true) && (removeCurrentWord == false)) {
            if(emailRe.match(currentWord).hasMatch() == true) {
                removeCurrentWord = true;
            }
        }

        /* Attempt to remove website addresses using the websiteRe Regular Expression. */
        if((d->settings->removeWebsites == true) && (removeCurrentWord == false)) {
            if(websiteRe.match(currentWord).hasMatch() == true) {
                removeCurrentWord = true;
            } else if (currentWord.contains(websiteCharsRe) == true) {
                QStringList wordsSplitOnWebChars = currentWord.split(websiteCharsRe, QString::SkipEmptyParts);
                if (wordsSplitOnWebChars.isEmpty() == false) {
                    /* String is not a website, check each component now */
                    removeCurrentWord = true;
                    WordList wordsFromSplit;
                    IDocumentParser::getWordsFromSplitString(wordsSplitOnWebChars, (*iter), wordsFromSplit);
                    /* Apply the settings to the words that came from the split to filter out words that does
                     * not belong due to settings. After they have passed the settings, add the words that survived
                     * to the list of words that should be added in the end */
                    applySettingsToWords(string, wordsFromSplit, wordsInSource);
                    wordsToAddInTheEnd.append(wordsFromSplit);
                }
            }
        }

        if((d->settings->checkAllCapsWords == false) && (removeCurrentWord == false)) {
            /* Remove words that are all caps */
            if(currentWord == currentWordCaps) {
                removeCurrentWord = true;
            }
        }

        if((d->settings->wordsWithNumberOption != CppParserSettings::LeaveWordsWithNumbers) && (removeCurrentWord == false)) {
            /* Before doing anything, check if the word contains any numbers. If it does then we can go to the
             * settings to handle the word differently */
            static const QRegularExpression numberContainRe(QStringLiteral("[0-9]"));
            static const QRegularExpression numberSplitRe(QStringLiteral("[0-9]+"));
            if(currentWord.contains(numberContainRe) == true) {
                /* Handle words with numbers based on the setting that is set for them */
                if(d->settings->wordsWithNumberOption == CppParserSettings::RemoveWordsWithNumbers) {
                    removeCurrentWord = true;
                } else if(d->settings->wordsWithNumberOption == CppParserSettings::SplitWordsOnNumbers) {
                    removeCurrentWord = true;
                    QStringList wordsSplitOnNumbers = currentWord.split(numberSplitRe, QString::SkipEmptyParts);
                    WordList wordsFromSplit;
                    IDocumentParser::getWordsFromSplitString(wordsSplitOnNumbers, (*iter), wordsFromSplit);
                    /* Apply the settings to the words that came from the split to filter out words that does
                     * not belong due to settings. After they have passed the settings, add the words that survived
                     * to the list of words that should be added in the end */
                    applySettingsToWords(string, wordsFromSplit, wordsInSource);
                    wordsToAddInTheEnd.append(wordsFromSplit);
                } else {
                    /* Should never get here */
                    QTC_CHECK(false);
                }
            }
        }

        if((d->settings->wordsWithUnderscoresOption != CppParserSettings::LeaveWordsWithUnderscores) && (removeCurrentWord == false)) {
            /* Check to see if the word has underscores in it. If it does then handle according to the settings */
            if(currentWord.contains(QLatin1Char('_')) == true) {
                if(d->settings->wordsWithUnderscoresOption == CppParserSettings::RemoveWordsWithUnderscores) {
                    removeCurrentWord = true;
                } else if(d->settings->wordsWithUnderscoresOption == CppParserSettings::SplitWordsOnUnderscores) {
                    removeCurrentWord = true;
                    static const QRegularExpression underscoreSplitRe(QStringLiteral("_+"));
                    QStringList wordsSplitOnUnderScores = currentWord.split(underscoreSplitRe, QString::SkipEmptyParts);
                    WordList wordsFromSplit;
                    IDocumentParser::getWordsFromSplitString(wordsSplitOnUnderScores, (*iter), wordsFromSplit);
                    /* Apply the settings to the words that came from the split to filter out words that does
                     * not belong due to settings. After they have passed the settings, add the words that survived
                     * to the list of words that should be added in the end */
                    applySettingsToWords(string, wordsFromSplit, wordsInSource);
                    wordsToAddInTheEnd.append(wordsFromSplit);
                } else {
                    /* Should never get here */
                    QTC_CHECK(false);
                }
            }
        }

        /* Settings for CamelCase */
        if((d->settings->camelCaseWordOption != CppParserSettings::LeaveWordsInCamelCase) && (removeCurrentWord == false)) {
            /* Check to see if the word appears to be in camelCase. If it does, handle according to the settings */
            /* The check is not precise and accurate science, but a rough estimation of the word is in camelCase. This
             * will probably be updated as this gets tested. The current check checks for one or more lower case letters,
             * followed by one or more upper-case letter, followed by a lower case letter */
            static const QRegularExpression camelCaseContainsRe(QStringLiteral("[a-z]{1,}[A-Z]{1,}[a-z]{1,}"));
            static const QRegularExpression camelCaseIndexRe(QStringLiteral("[a-z][A-Z]"));
            if(currentWord.contains(camelCaseContainsRe) == true ) {
                if(d->settings->camelCaseWordOption == CppParserSettings::RemoveWordsInCamelCase) {
                    removeCurrentWord = true;
                } else if(d->settings->camelCaseWordOption == CppParserSettings::SplitWordsOnCamelCase) {
                    removeCurrentWord = true;
                    QStringList wordsSplitOnCamelCase;
                    /* Search the word for all indexes where there is a lower case letter followed by an upper case
                     * letter. This indexes are then later used to split the current word into a list of new words.
                     * 0 is added as the starting index, since the first word will start at 0. At the end the length
                     * of the word is also added, since the last word will stop at the end */
                    QList<int> indexes;
                    indexes << 0;
                    int currentIdx = 0;
                    int lastIdx = 0;
                    bool finished = false;
                    while(finished == false) {
                        currentIdx = currentWord.indexOf(camelCaseIndexRe, lastIdx);
                        if(currentIdx == -1) {
                            finished = true;
                            indexes << currentWord.length();
                        } else {
                            lastIdx = currentIdx + 1;
                            indexes << lastIdx;
                        }
                    }
                    /* Now split the word on the indexes */
                    for(int idx = 0; idx < indexes.count() - 1; ++idx) {
                        /* Get the word starting at the current index, up to the difference between the different
                         * index and the current index, since the second argument of QString::mid() is the length
                         * to extract and not the index of the last position */
                        QString word = currentWord.mid(indexes.at(idx), indexes.at(idx + 1) - indexes.at(idx));
                        wordsSplitOnCamelCase << word;
                    }
                    WordList wordsFromSplit;
                    /* Get the proper word structures for the words extracted during the split */
                    IDocumentParser::getWordsFromSplitString(wordsSplitOnCamelCase, (*iter), wordsFromSplit);
                    /* Apply the settings to the words that came from the split to filter out words that does
                     * not belong due to settings. After they have passed the settings, add the words that survived
                     * to the list of words that should be added in the end */
                    applySettingsToWords(string, wordsFromSplit, wordsInSource);
                    wordsToAddInTheEnd.append(wordsFromSplit);
                } else {
                    /* Should never get here */
                    QTC_CHECK(false);
                }
            }
        }

        /* Words.with.dots */
        if((d->settings->wordsWithDotsOption != CppParserSettings::LeaveWordsWithDots) && (removeCurrentWord == false)) {
            /* Check to see if the word has dots in it. If it does then handle according to the settings */
            if(currentWord.contains(QLatin1Char('.')) == true) {
                if(d->settings->wordsWithDotsOption == CppParserSettings::RemoveWordsWithDots) {
                    removeCurrentWord = true;
                } else if(d->settings->wordsWithDotsOption == CppParserSettings::SplitWordsOnDots) {
                    removeCurrentWord = true;
                    static const QRegularExpression dotsSplitRe(QStringLiteral("\\.+"));
                    QStringList wordsSplitOnDots = currentWord.split(dotsSplitRe, QString::SkipEmptyParts);
                    WordList wordsFromSplit;
                    IDocumentParser::getWordsFromSplitString(wordsSplitOnDots, (*iter), wordsFromSplit);
                    /* Apply the settings to the words that came from the split to filter out words that does
                     * not belong due to settings. After they have passed the settings, add the words that survived
                     * to the list of words that should be added in the end */
                    applySettingsToWords(string, wordsFromSplit, wordsInSource);
                    wordsToAddInTheEnd.append(wordsFromSplit);
                } else {
                    /* Should never get here */
                    QTC_CHECK(false);
                }
            }
        }

        /* Remove the current word if it should be removed. The word will get removed in place. The
         * erase() function on the list will return an iterator to the next element. In this case,
         * the iterator should not be incremented and the while loop should continue to the next element. */
        if(removeCurrentWord == true) {
            iter = words.erase(iter);
        } else {
            ++iter;
        }
    }
    /* Add the words that should be added in the end to the list of words */
    words.append(wordsToAddInTheEnd);
}
//--------------------------------------------------

//--------------------------------------------------

bool CppDocumentParser::isReservedWord(const QString &word)
{
    /* Trying to optimize the check using the same method as used
     * in the cpptoolsreuse.cpp file in the CppTools plugin. */
    switch (word.length()) {
    case 3:
        switch(word.at(0).toUpper().toLatin1()) {
        case 'C':
            if(word.toUpper() == QStringLiteral("CPP"))
                return true;
            break;
        case 'S':
            if(word.toUpper() == QStringLiteral("STD"))
                return true;
            break;
        }
        break;
    case 4:
        switch(word.at(0).toUpper().toLatin1()) {
        case 'E':
            if(word.toUpper() == QStringLiteral("ENUM"))
                return true;
            break;
        }
        break;
    case 6:
        switch(word.at(0).toUpper().toLatin1()) {
        case 'S':
            if(word.toUpper() == QStringLiteral("STRUCT"))
                return true;
            break;
        case 'P':
            if(word.toUpper() == QStringLiteral("PLUGIN"))
                return true;
          break;
        }
        break;
    case 7:
        switch(word.at(0).toUpper().toLatin1()) {
        case 'D':
            if(word.toUpper() == QStringLiteral("DOXYGEN"))
                return true;
            break;
        case 'N':
            if(word.toUpper() == QStringLiteral("NULLPTR"))
                return true;
            break;
        case 'T':
            if(word.toUpper() == QStringLiteral("TYPEDEF"))
                return true;
            break;
        }
        break;
    case 9:
        switch(word.at(0).toUpper().toLatin1()) {
        case 'N':
            if(word.toUpper() == QStringLiteral("NAMESPACE"))
                return true;
            break;
        }
        break;
    default:
        break;
    }
    return false;
}
//--------------------------------------------------

} // namespace Internal
} // namespace CppSpellChecker
} // namespace SpellChecker
