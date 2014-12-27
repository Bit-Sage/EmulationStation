#include "GamelistDB.h"
#include "MetaData.h"
#include "Log.h"
#include "SystemData.h"
#include <sstream>

namespace fs = boost::filesystem;

std::string pathToFileID(const fs::path& path, const fs::path& systemStartPath)
{
	return makeRelativePath(path, systemStartPath, false).generic_string();
}

std::string pathToFileID(const fs::path& path, const SystemData* system)
{
	return pathToFileID(path, system->getStartPath());
}

fs::path fileIDToPath(const std::string& fileID, const SystemData* system)
{
	return resolvePath(fileID, system->getStartPath(), true);
}


// super simple RAII wrapper for sqlite3
// prepared statement, can be used just like an sqlite3_stmt* thanks to overloaded operator
class SQLPreparedStmt
{
public:
	SQLPreparedStmt(sqlite3* db, const char* stmt) : mDB(db), mStmt(NULL) {
		if(sqlite3_prepare_v2(db, stmt, strlen(stmt), &mStmt, NULL))
			throw DBException() << "Error creating prepared stmt \"" << stmt << "\".\n\t" << sqlite3_errmsg(db);
	}

	SQLPreparedStmt(sqlite3* db, const std::string& stmt) : SQLPreparedStmt(db, stmt.c_str()) {};

	int step() { return sqlite3_step(mStmt); }

	void step_expected(int expected) {
		if(step() != expected)
			throw DBException() << "Step failed!\n\t" << sqlite3_errmsg(mDB);
	}

	void reset() { 
		if(sqlite3_reset(mStmt))
			throw DBException() << "Error resetting statement!\n\t" << sqlite3_errmsg(mDB);
	}

	~SQLPreparedStmt() {
		if(mStmt)
			sqlite3_finalize(mStmt);
	}

	operator sqlite3_stmt*() { return mStmt; }

private:
	sqlite3* mDB; // used for error messages
	sqlite3_stmt* mStmt;
};

// encapsulates a transaction that cannot outlive the lifetime of this object
class SQLTransaction
{
public:
	SQLTransaction(sqlite3* db) : mDB(db) {
		if(sqlite3_exec(mDB, "BEGIN TRANSACTION", NULL, NULL, NULL))
			throw DBException() << "Error beginning transaction.\n\t" << sqlite3_errmsg(mDB);
	}

	void commit() {
		if(!mDB)
			throw DBException() << "Transaction already committed!";

		if(sqlite3_exec(mDB, "COMMIT TRANSACTION", NULL, NULL, NULL))
			throw DBException() << "Error committing transaction.\n\t" << sqlite3_errmsg(mDB);

		mDB = NULL;
	}

	~SQLTransaction() {
		if(mDB)
			commit();
	}

private:
	sqlite3* mDB;
};


GamelistDB::GamelistDB(const std::string& path) : mDB(NULL)
{
	openDB(path.c_str());
}

GamelistDB::~GamelistDB()
{
	closeDB();
}

void GamelistDB::openDB(const char* path)
{
	if(sqlite3_open(path, &mDB))
	{
		throw DBException() << "Could not open database \"" << path << "\".\n"
			"\t" << sqlite3_errmsg(mDB);
	}

	createTables();
}

void GamelistDB::closeDB()
{
	if(mDB)
	{
		sqlite3_close(mDB);
		mDB = NULL;
	}
}

void GamelistDB::createTables()
{
	auto decl_type = GAME_METADATA;
	const std::vector<MetaDataDecl>& decl = getMDDMap().at(decl_type);

	std::stringstream ss;
	ss << "CREATE TABLE IF NOT EXISTS files (" <<
		"fileid VARCHAR(255) NOT NULL, " <<
		"systemid VARCHAR(255) NOT NULL, " <<
		"filetype INT NOT NULL, " <<
		"fileexists BOOLEAN, ";
	for(auto it = decl.begin(); it != decl.end(); it++)
	{
		// format here is "[key] [type] DEFAULT [default_value],"
		ss << it->key << " ";

		// metadata type -> SQLite type
		switch(it->type)
		{
		case MD_IMAGE_PATH:
		case MD_MULTILINE_STRING:
		case MD_STRING:
			ss << "VARCHAR(255)";
			if(!it->defaultValue.empty())
				ss << " DEFAULT '" << it->defaultValue << "'";
			break;

		case MD_INT:
			ss << "INT";
			if(!it->defaultValue.empty())
				ss << " DEFAULT '" << it->defaultValue << "'";
			break;

		case MD_RATING:
		case MD_FLOAT:
			ss << "FLOAT";
			if(!it->defaultValue.empty())
				ss << " DEFAULT '" << it->defaultValue << "'";
			break;

		case MD_DATE:
			ss << "DATE"; // TODO default
			break;
		case MD_TIME:
			ss << "DATETIME"; // TODO default
			break;
		}

		ss << ", ";
	}

	ss << "PRIMARY KEY (fileid, systemid))";

	if(sqlite3_exec(mDB, ss.str().c_str(), NULL, NULL, NULL))
		throw DBException() << "Error creating table!\n\t" << sqlite3_errmsg(mDB);
}

// what this does:
// - if we add at least one valid file in this folder, return true.
// - given a folder (start_dir), go through all the files and folders in it.
// - if it's a file, check if its extension matches our list (extensions),
//   adding it to the "files" table if it does (filetype = game).
//   also mark this folder as having a file.
// - if it's a folder, recurse into it. if that recursion returns true, also mark this folder as having a file.
// - if this folder is marked as having a file at return time, add it to the database.

void add_file(const char* fileid, int filetype, sqlite3* db, sqlite3_stmt* insert_stmt)
{
	if(sqlite3_bind_text(insert_stmt, 1, fileid, strlen(fileid), SQLITE_STATIC))
		throw DBException() << "Error binding fileid in populate().\n\t" << sqlite3_errmsg(db);

	if(sqlite3_bind_int(insert_stmt, 2, FileType::GAME))
		throw DBException() << "Error binding filetype in populate().\n\t" << sqlite3_errmsg(db);

	if(sqlite3_step(insert_stmt) != SQLITE_DONE)
		throw DBException() << "Error adding file \"" << fileid << "\" in populate().\n\t" << sqlite3_errmsg(db);

	if(sqlite3_reset(insert_stmt))
		throw DBException() << "Error resetting statement for \"" << fileid << "\" in populate().\n\t" << sqlite3_errmsg(db);
}

bool populate_recursive(const fs::path& relativeTo, const std::vector<std::string>& extensions, 
	const fs::path& start_dir, sqlite3* db, sqlite3_stmt* insert_stmt)
{
	bool contains; // ignored

	bool has_a_file = false;
	for(fs::directory_iterator end, dir(start_dir); dir != end; ++dir)
	{
		if(fs::is_directory(*dir))
		{
			if(populate_recursive(relativeTo, extensions, *dir, db, insert_stmt))
				has_a_file = true;

			continue;
		}

		fs::path path = *dir;
		if(std::find(extensions.begin(), extensions.end(), path.extension()) == extensions.end())
			continue;

		const std::string fileid = pathToFileID(path, relativeTo);
		add_file(fileid.c_str(), FileType::GAME, db, insert_stmt);

		has_a_file = true;
	}

	if(has_a_file)
	{
		// this folder had a game, add it to the DB
		std::string fileid = pathToFileID(start_dir, relativeTo);
		add_file(fileid.c_str(), FileType::FOLDER, db, insert_stmt);
	}

	return has_a_file;
}

void GamelistDB::addMissingFiles(const SystemData* system)
{
	const std::string& relativeTo = system->getStartPath(); 
	const std::vector<std::string>& extensions = system->getExtensions();

	// ?1 = fileid, ?2 = filetype
	std::stringstream ss;
	ss << "INSERT OR IGNORE INTO files (fileid, systemid, filetype) VALUES (?1, " << system->getName() << ", ?2)";
	std::string insert = ss.str();
	
	SQLPreparedStmt stmt(mDB, insert);
	SQLTransaction transaction(mDB);

	// actually start adding things
	populate_recursive(relativeTo, extensions, relativeTo, mDB, stmt);

	transaction.commit();
}

void GamelistDB::updateExists(const SystemData* system)
{
	const std::string& relativeTo = system->getStartPath();

	std::stringstream ss;
	ss << "SELECT fileid FROM files WHERE systemid = " << system->getName();
	std::string readstr = ss.str();

	ss.str("");
	ss << "UPDATE files SET fileexists = ?1 WHERE fileid = ?2 AND systemid = " << system->getName();
	std::string updatestr = ss.str();

	SQLPreparedStmt readStmt(mDB, readstr);
	SQLPreparedStmt updateStmt(mDB, updatestr);

	SQLTransaction transaction(mDB);

	// for each game, check if its file exists - if it doesn't remove it from the DB
	while(readStmt.step() != SQLITE_DONE)
	{
		const char* path = (const char*)sqlite3_column_text(readStmt, 0);

		bool exists = false;
		if(path && path[0] == '.') // it's relative
			exists = fs::exists(relativeTo + "/" + path);
		else
			exists = fs::exists(path);

		sqlite3_bind_text(updateStmt, 2, path, strlen(path), SQLITE_STATIC);
		sqlite3_bind_int(updateStmt, 1, exists);
		updateStmt.step_expected(SQLITE_DONE);
		updateStmt.reset();
	}

	transaction.commit();
}

MetaDataMap GamelistDB::getFileData(const std::string& fileID, const std::string& systemID) const
{
	SQLPreparedStmt readStmt(mDB, "SELECT * FROM files WHERE fileid = ?1 AND systemid = ?2");
	sqlite3_bind_text(readStmt, 1, fileID.c_str(), fileID.size(), SQLITE_STATIC);
	sqlite3_bind_text(readStmt, 2, systemID.c_str(), systemID.size(), SQLITE_STATIC);

	readStmt.step_expected(SQLITE_DONE);

	MetaDataListType type = sqlite3_column_int(readStmt, 1) ? FOLDER_METADATA : GAME_METADATA;
	MetaDataMap mdl(type);

	for(int i = 2; i < sqlite3_column_count(readStmt); i++)
	{
		const char* col = (const char*)sqlite3_column_name(readStmt, i);
		const char* value = (const char*)sqlite3_column_text(readStmt, i);

		mdl.set(col, value);
	}

	return mdl;
}

void GamelistDB::setFileData(const std::string& fileID, const std::string& systemID, const MetaDataMap& metadata)
{
	std::stringstream ss;
	ss << "INSERT OR REPLACE INTO files VALUES (?1, ?2, ?3, ?4, ";

	auto& mdd = metadata.getMDD();
	for(unsigned int i = 0; i < mdd.size(); i++)
	{
		ss << "?" << i + 5;

		if(i + 1 < mdd.size())
			ss << ", ";
	}
	ss << ")";

	std::string insertstr = ss.str();
	SQLPreparedStmt stmt(mDB, insertstr.c_str());
	sqlite3_bind_text(stmt, 1, fileID.c_str(), fileID.size(), SQLITE_STATIC); // fileid
	sqlite3_bind_text(stmt, 2, systemID.c_str(), systemID.size(), SQLITE_STATIC); // systemid
	sqlite3_bind_int(stmt, 3, metadata.getType() == FOLDER_METADATA); // filetype
	sqlite3_bind_int(stmt, 4, 1); // fileexists

	for(unsigned int i = 0; i < mdd.size(); i++)
	{
		const std::string& val = metadata.get(mdd.at(i).key);
		sqlite3_bind_text(stmt, i + 5, val.c_str(), val.size(), SQLITE_STATIC);
	}

	stmt.step_expected(SQLITE_DONE);
}

void GamelistDB::importXML(const SystemData* system, const std::string& xml_path)
{
	LOG(LogInfo) << "Appending gamelist.xml file \"" << xml_path << "\" to database (system: " << system->getName() << ")...";

	if(!fs::exists(xml_path))
		throw ESException() << "XML file not found (path: " << xml_path << ")";

	pugi::xml_document doc;
	pugi::xml_parse_result result = doc.load_file(xml_path.c_str());
	if(!result)
		throw ESException() << "Error parsing XML:\n\t" << result.description();

	pugi::xml_node root = doc.child("gameList");
	if(!root)
		throw ESException() << "Could not find <gameList> node!";

	const fs::path relativeTo = system->getStartPath();
	
	unsigned int skipCount = 0;
	const char* tagList[2] = { "game", "folder" };
	MetaDataListType typeList[2] = { GAME_METADATA, FOLDER_METADATA };

	for(int i = 0; i < 2; i++)
	{
		const char* tag = tagList[i];
		MetaDataListType type = typeList[i];

		for(pugi::xml_node fileNode = root.child(tag); fileNode; fileNode = fileNode.next_sibling(tag))
		{
			fs::path path = resolvePath(fileNode.child("path").text().get(), relativeTo, false);

			if(!boost::filesystem::exists(path))
			{
				LOG(LogWarning) << "File \"" << path << "\" does not exist! Ignoring.";
				skipCount++;
				continue;
			}

			// make a metadata map
			MetaDataMap mdl(type);
			const std::vector<MetaDataDecl>& mdd = mdl.getMDD();
			for(auto iter = mdd.begin(); iter != mdd.end(); iter++)
			{
				pugi::xml_node md = fileNode.child(iter->key.c_str());
				if(md)
				{
					// if it's a path, resolve relative paths
					std::string value = md.text().get();
					if(iter->type == MD_IMAGE_PATH)
						value = resolvePath(value, relativeTo, true).generic_string();

					mdl.set(iter->key, value);
				}
			}

			// make sure the name is set
			assert(!mdl.get<std::string>("name").empty());

			this->setFileData(pathToFileID(path, system), system->getName(), mdl);
		}
	}
}

void GamelistDB::exportXML(const SystemData* system, const std::string& xml_path)
{
	pugi::xml_document doc;
	pugi::xml_node root = doc.append_child("gameList");

	SQLPreparedStmt readStmt(mDB, "SELECT * FROM files WHERE systemid = ?1");
	sqlite3_bind_text(readStmt, 1, system->getName().c_str(), system->getName().size(), SQLITE_STATIC);
	
	std::string relativeTo = system->getStartPath();
	while(readStmt.step() != SQLITE_DONE)
	{
		MetaDataListType type = sqlite3_column_int(readStmt, 2) ? FOLDER_METADATA : GAME_METADATA;
		pugi::xml_node node = root.append_child(type == GAME_METADATA ? "game" : "folder");

		// write path
		std::string path = (const char*)sqlite3_column_text(readStmt, 0);
		if(path[0] == '.')
			path = relativeTo + path.substr(1, std::string::npos);

		node.append_child("path").text().set(path.c_str());

		// skip column 0 (fileid), 1 (systemid), 2 (filetype), 3 (fileexists)
		for(int i = 4; i < sqlite3_column_count(readStmt); i++)
		{
			const char* col = (const char*)sqlite3_column_name(readStmt, i);
			const char* value = (const char*)sqlite3_column_text(readStmt, i);
			node.append_child(col).text().set(value);
		}
	}

	doc.save_file(xml_path.c_str());
}