//////////////////////////////////////////////////////////////////////
// The Forgotten Server - a server application for the MMORPG Tibia
//////////////////////////////////////////////////////////////////////
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//////////////////////////////////////////////////////////////////////

#include "otpch.h"

#include "configmanager.h"
#include "database.h"
#include <string>

#if defined(WIN32) && !defined(_MSC_VER)
#include <mysql/errmsg.h>
#else
#include <errmsg.h>
#endif

extern ConfigManager g_config;

Database* Database::_instance = NULL;

Database::Database()
{
	m_connected = false;

	// connection handle initialization
	m_handle = mysql_init(NULL);
	if (!m_handle) {
		std::cout << std::endl << "Failed to initialize MySQL connection handle." << std::endl;
		return;
	}

	// automatic reconnect
	my_bool reconnect = true;
	mysql_options(m_handle, MYSQL_OPT_RECONNECT, &reconnect);

	// connects to database
	if (!mysql_real_connect(m_handle, g_config.getString(ConfigManager::MYSQL_HOST).c_str(), g_config.getString(ConfigManager::MYSQL_USER).c_str(), g_config.getString(ConfigManager::MYSQL_PASS).c_str(), g_config.getString(ConfigManager::MYSQL_DB).c_str(), g_config.getNumber(ConfigManager::SQL_PORT), NULL, 0)) {
		std::cout << "Failed to connect to database. MYSQL ERROR: " << mysql_error(m_handle) << std::endl;
		return;
	}

	if (MYSQL_VERSION_ID < 50019) {
		//mySQL servers < 5.0.19 has a bug where MYSQL_OPT_RECONNECT is (incorrectly) reset by mysql_real_connect calls
		//See http://dev.mysql.com/doc/refman/5.0/en/mysql-options.html for more information.
		mysql_options(m_handle, MYSQL_OPT_RECONNECT, &reconnect);
		std::cout << std::endl << "[Warning] Outdated MySQL server detected. Consider upgrading to a newer version." << std::endl;
	}

	m_connected = true;

	if (g_config.getString(ConfigManager::MAP_STORAGE_TYPE) == "binary") {
		DBResult* result = storeQuery("SHOW variables LIKE 'max_allowed_packet'");
		if (result) {
			int32_t max_query = result->getDataInt("Value");
			freeResult(result);

			if (max_query < 16777216) {
				std::cout << std::endl << "[Warning] max_allowed_packet might be set too low for binary map storage." << std::endl;
				std::cout << "Use the following query to raise max_allow_packet: ";
				std::cout << "SET GLOBAL max_allowed_packet = 16777216";
			}
		}
	}
}

Database::~Database()
{
	mysql_close(m_handle);
}

Database* Database::getInstance()
{
	if (!_instance) {
		_instance = new Database;
	}
	return _instance;
}

bool Database::beginTransaction()
{
	database_lock.lock();
	return executeQuery("BEGIN");
}

bool Database::rollback()
{
	if (!m_connected) {
		database_lock.unlock();
		return false;
	}

	if (mysql_rollback(m_handle) != 0) {
		std::cout << "mysql_rollback(): MYSQL ERROR: " << mysql_error(m_handle) << std::endl;
		database_lock.unlock();
		return false;
	}

	database_lock.unlock();
	return true;
}

bool Database::commit()
{
	if (!m_connected) {
		database_lock.unlock();
		return false;
	}

	if (mysql_commit(m_handle) != 0) {
		std::cout << "mysql_commit(): MYSQL ERROR: " << mysql_error(m_handle) << std::endl;
		database_lock.unlock();
		return false;
	}

	database_lock.unlock();
	return true;
}

bool Database::executeQuery(const std::string& query)
{
	if (!m_connected) {
		return false;
	}

	bool state = true;

	// executes the query
	database_lock.lock();
	if (mysql_real_query(m_handle, query.c_str(), query.length()) != 0) {
		std::cout << "mysql_real_query(): " << query.substr(0, 256) << ": MYSQL ERROR: " << mysql_error(m_handle) << std::endl;

		int error = mysql_errno(m_handle);
		if (error == CR_SERVER_LOST || error == CR_SERVER_GONE_ERROR) {
			m_connected = false;
		}

		state = false;
	}

	// we should call that every time as someone would call executeQuery('SELECT...')
	// as it is described in MySQL manual: "it doesn't hurt" :P
	MYSQL_RES* m_res = mysql_store_result(m_handle);
	database_lock.unlock();

	if (m_res) {
		mysql_free_result(m_res);
	}

	return state;
}

DBResult* Database::storeQuery(const std::string& query)
{
	if (!m_connected) {
		return NULL;
	}

	// executes the query
	database_lock.lock();
	if (mysql_real_query(m_handle, query.c_str(), query.length()) != 0) {
		std::cout << "mysql_real_query(): " << query << ": MYSQL ERROR: " << mysql_error(m_handle) << std::endl;
		int error = mysql_errno(m_handle);
		if (error == CR_SERVER_LOST || error == CR_SERVER_GONE_ERROR) {
			m_connected = false;
		}
	}

	// we should call that every time as someone would call executeQuery('SELECT...')
	// as it is described in MySQL manual: "it doesn't hurt" :P
	MYSQL_RES* m_res = mysql_store_result(m_handle);

	// error occured
	if (!m_res) {
		std::cout << "mysql_store_result(): " << query.substr(0, 256) << ": MYSQL ERROR: " << mysql_error(m_handle) << std::endl;
		int error = mysql_errno(m_handle);
		database_lock.unlock();

		if (error == CR_SERVER_LOST || error == CR_SERVER_GONE_ERROR) {
			m_connected = false;
		}
		return NULL;
	}
	database_lock.unlock();

	// retriving results of query
	return verifyResult(new DBResult(m_res));
}

std::string Database::escapeString(const std::string& s) const
{
	return escapeBlob(s.c_str(), s.length());
}

std::string Database::escapeBlob(const char* s, uint32_t length) const
{
	if (!s) {
		return std::string("''");
	}

	// the worst case is 2n + 1
	char* output = new char[length * 2 + 1];

	// quotes escaped string and frees temporary buffer
	mysql_real_escape_string(m_handle, output, s, length);
	std::string r = "'";
	r += output;
	r += "'";
	delete[] output;
	return r;
}

void Database::freeResult(DBResult* res)
{
	delete res;
}

DBResult::DBResult(MYSQL_RES* res)
{
	m_handle = res;
	m_listNames.clear();

	int32_t i = 0;

	MYSQL_FIELD* field = mysql_fetch_field(m_handle);
	while (field) {
		m_listNames[field->name] = i++;
		field = mysql_fetch_field(m_handle);
	}
}

DBResult::~DBResult()
{
	mysql_free_result(m_handle);
}

DBResult* Database::verifyResult(DBResult* result)
{
	if (!result->next()) {
		_instance->freeResult(result);
		return NULL;
	}
	return result;
}

int32_t DBResult::getDataInt(const std::string& s) const
{
	listNames_t::const_iterator it = m_listNames.find(s);
	if (it == m_listNames.end()) {
		std::cout << "Error during getDataInt(" << s << ")." << std::endl;
		return 0;
	}

	if (m_row[it->second] == NULL) {
		return 0;
	}

	return atoi(m_row[it->second]);
}

int64_t DBResult::getDataLong(const std::string& s) const
{
	listNames_t::const_iterator it = m_listNames.find(s);
	if (it == m_listNames.end()) {
		std::cout << "Error during getDataLong(" << s << ")." << std::endl;
		return 0;
	}

	if (m_row[it->second] == NULL) {
		return 0;
	}

	return ATOI64(m_row[it->second]);
}

std::string DBResult::getDataString(const std::string& s) const
{
	listNames_t::const_iterator it = m_listNames.find(s);
	if (it == m_listNames.end()) {
		std::cout << "Error during getDataString(" << s << ")." << std::endl;
		return std::string("");
	}

	if (m_row[it->second] == NULL) {
		return std::string("");
	}

	return std::string(m_row[it->second]);
}

const char* DBResult::getDataStream(const std::string& s, unsigned long& size) const
{
	listNames_t::const_iterator it = m_listNames.find(s);
	if (it == m_listNames.end()) {
		std::cout << "Error during getDataStream(" << s << ")." << std::endl;
		size = 0;
		return NULL;
	}

	if (m_row[it->second] == NULL) {
		size = 0;
		return NULL;
	}

	size = mysql_fetch_lengths(m_handle)[it->second];
	return m_row[it->second];
}

bool DBResult::next()
{
	m_row = mysql_fetch_row(m_handle);
	return m_row != NULL;
}

DBInsert::DBInsert(Database* db)
{
	m_db = db;
	m_rows = 0;
}

void DBInsert::setQuery(const std::string& query)
{
	m_query = query;
	m_buf = "";
	m_rows = 0;
}

bool DBInsert::addRow(const std::string& row)
{
	m_rows++;

	// adds new row to buffer
	size_t size = m_buf.length();
	if (size == 0) {
		m_buf = "(" + row + ")";
	} else if (size > 8192) {
		if (!execute()) {
			return false;
		}

		m_buf = "(" + row + ")";
	} else {
		m_buf += ",(" + row + ")";
	}
	return true;
}

bool DBInsert::addRow(std::ostringstream& row)
{
	bool ret = addRow(row.str());
	row.str("");
	return ret;
}

bool DBInsert::execute()
{
	if (m_buf.empty() || m_rows == 0) {
		return true;
	}

	m_rows = 0;

	// executes buffer
	bool res = m_db->executeQuery(m_query + m_buf);
	m_buf = "";
	return res;
}
