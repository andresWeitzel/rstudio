/*
 * DatabaseTests.cpp
 *
 * Copyright (C) 2020 by RStudio, PBC
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include <tests/TestThat.hpp>

#include <core/Database.hpp>
#include <core/FileSerializer.hpp>
#include <core/system/System.hpp>
#include <shared_core/SafeConvert.hpp>

#include <soci/boost-tuple.h>
#include <soci/session.h>
#include <soci/sqlite3/soci-sqlite3.h>

namespace rstudio {
namespace unit_tests {

using namespace core;
using namespace core::database;

core::database::SqliteConnectionOptions sqliteConnectionOptions()
{
   return SqliteConnectionOptions { "/tmp/rstudio-test-db" };
}

core::database::PostgresqlConnectionOptions postgresConnectionOptions()
{
   PostgresqlConnectionOptions options;
   options.connectionTimeoutSeconds = 10;
   options.database = "rstudio-test";
   options.host = "localhost";
   options.user = "postgres";
   options.password = "postgres";

   return options;
}

TEST_CASE("Database", "[.database]")
{
   test_that("Test Setup")
   {
      // ensure that the test databases do not exist
      FilePath sqliteDbPath("/tmp/rstudio-test-db");
      sqliteDbPath.removeIfExists();

      boost::shared_ptr<Connection> connection;
      Error error = connect(postgresConnectionOptions(), &connection);
      if (error)
         return;

      std::string queryStr =
         R""(
         DROP SCHEMA public CASCADE;
         CREATE SCHEMA public;
         GRANT ALL ON SCHEMA public TO postgres;
         GRANT ALL ON SCHEMA public TO public;
         )"";

      connection->executeStr(queryStr);
   }

   test_that("Can create SQLite database")
   {
      boost::shared_ptr<Connection> connection;
      REQUIRE_FALSE(connect(sqliteConnectionOptions(), &connection));

      Query query = connection->query("create table Test(id int, text varchar(255))");
      REQUIRE_FALSE(connection->execute(query));

      int id = 10;
      std::string text = "Hello, database!";

      query = connection->query("insert into Test(id, text) values(:id, :text)")
            .withInput(id)
            .withInput(text);
      REQUIRE_FALSE(connection->execute(query));

      boost::tuple<int, std::string> row;
      query = connection->query("select id, text from Test where id = (:id)")
            .withInput(id)
            .withOutput(row);
      REQUIRE_FALSE(connection->execute(query));

      CHECK(row.get<0>() == id);
      CHECK(row.get<1>() == text);
   }

   test_that("Can create PostgreSQL database")
   {
      boost::shared_ptr<Connection> connection;
      REQUIRE_FALSE(connect(postgresConnectionOptions(), &connection));

      Query query = connection->query("create table Test(id int, text varchar(255))");
      REQUIRE_FALSE(connection->execute(query));

      int id = 10;
      std::string text = "Hello, database!";

      query = connection->query("insert into Test(id, text) values(:id, :text)")
            .withInput(id)
            .withInput(text);
      REQUIRE_FALSE(connection->execute(query));

      boost::tuple<int, std::string> row;
      query = connection->query("select id, text from Test where id = (:id)")
            .withInput(id)
            .withOutput(row);
      REQUIRE_FALSE(connection->execute(query));

      CHECK(row.get<0>() == id);
      CHECK(row.get<1>() == text);
   }

   test_that("Can perform transactions")
   {
      boost::shared_ptr<Connection> connection;
      REQUIRE_FALSE(connect(sqliteConnectionOptions(), &connection));

      Transaction transaction(connection);
      int numFailed = 0;
      bool dataReturned = false;

      // verify that we can commit a transaction
      Query query = connection->query("insert into Test(id, text) values(:id, :text)");
      for (int id = 0; id < 100; ++id)
      {
         std::string text = "Test text " + core::safe_convert::numberToString(id);
         query.withInput(id).withInput(text);

         if (connection->execute(query))
            ++numFailed;
      }

      REQUIRE(numFailed == 0);
      transaction.commit();

      boost::tuple<int, std::string> row;
      query = connection->query("select id, text from Test where id = 50")
            .withOutput(row);

      REQUIRE_FALSE(connection->execute(query, &dataReturned));
      REQUIRE(dataReturned);
      REQUIRE(row.get<0>() == 50);
      REQUIRE(row.get<1>() == "Test text 50");

      // now attempt to rollback a transaction
      Transaction transaction2(connection);
      query = connection->query("insert into Test(id, text) values(:id, :text)");
      for (int id = 100; id < 200; ++id)
      {
         std::string text = "Test text " + core::safe_convert::numberToString(id);
         query.withInput(id).withInput(text);

         if (connection->execute(query))
            ++numFailed;
      }

      REQUIRE(numFailed == 0);
      transaction2.rollback();

      query = connection->query("select id, text from Test where id = 150")
            .withOutput(row);

      // expect no data
      REQUIRE_FALSE(connection->execute(query, &dataReturned));
      REQUIRE_FALSE(dataReturned);
   }

   test_that("Can use connection pool")
   {
      boost::shared_ptr<ConnectionPool> connectionPool;
      REQUIRE_FALSE(createConnectionPool(5, sqliteConnectionOptions(), &connectionPool));

      boost::shared_ptr<PooledConnection> connection = connectionPool->getConnection();
      boost::tuple<int, std::string> row;
      Query query = connection->query("select id, text from Test where id = 50")
            .withOutput(row);

      bool dataReturned = false;
      REQUIRE_FALSE(connection->execute(query, &dataReturned));
      REQUIRE(dataReturned);

      boost::shared_ptr<PooledConnection> connection2 = connectionPool->getConnection();
      Query query2 = connection2->query("select id, text from Test where id = 25")
            .withOutput(row);

      dataReturned = false;
      REQUIRE_FALSE(connection2->execute(query2, &dataReturned));
      REQUIRE(dataReturned);
   }

   test_that("Can update schemas")
   {
      // generate some schema files
      std::string schema1 =
         R""(
         CREATE TABLE TestTable1_Persons(
            id int NOT NULL,
            first_name varchar(255),
            last_name varchar(255) NOT NULL,
            email_address varchar(255)
         );

         CREATE TABLE TestTable2_AccountHolders(
            id int,
            fk_person_id int
         );
         )"";

      // sqlite cannot alter tables very well, so adding constraints necessitates dropping
      // and re-creating the tables
      std::string schema2Sqlite =
         R""(
         CREATE TABLE TestTable1_Persons_new(
            id int NOT NULL,
            first_name varchar(255),
            last_name varchar(255),
            email_address varchar(255),
            PRIMARY KEY (id)
         );

         DROP TABLE TestTable1_Persons;
         ALTER TABLE TestTable1_Persons_new RENAME TO TestTable1_Persons;

         CREATE TABLE TestTable2_AccountHolders_new(
            id int,
            fk_person_id int,
            PRIMARY KEY (id),
            FOREIGN KEY (fk_person_id) REFERENCES TestTable1_Persons(id)
         );

         DROP TABLE TestTable2_AccountHolders;
         ALTER TABLE TestTable2_AccountHolders_new RENAME TO TestTable2_AccountHolders;
         )"";

      // postgresql supports modification of tables
      std::string schema2Postgresql =
         R""(
         ALTER TABLE TestTable1_Persons
         ADD PRIMARY KEY (id);

         ALTER TABLE TestTable2_AccountHolders
         ADD PRIMARY KEY (id);

         ALTER TABLE TestTable2_AccountHolders
         ADD FOREIGN KEY (fk_person_id) REFERENCES TestTable1_Persons(id);
         )"";

      std::string schema3Sqlite =
         R""(
         CREATE TABLE TestTable2_AccountHolders_new(
            id int,
            fk_person_id int,
            creation_time text,
            PRIMARY KEY (id),
            FOREIGN KEY (fk_person_id) REFERENCES TestTable1_Persons(id)
         );

         DROP TABLE TestTable2_AccountHolders;
         ALTER TABLE TestTable2_AccountHolders_new RENAME TO TestTable2_AccountHolders;
         )"";

      std::string schema3Postgresql =
         R""(
         ALTER TABLE TestTable2_AccountHolders
         ADD COLUMN creation_time text;
         )"";

      FilePath workingDir = core::system::currentWorkingDir(core::system::currentProcessId());
      FilePath outFile1 = workingDir.completeChildPath("1_InitialTables.sql");
      FilePath outFile2Sqlite = workingDir.completeChildPath("2_ConstraintsForInitialTables.sqlite");
      FilePath outFile2Postgresql = workingDir.completeChildPath("2_ConstraintsForInitialTables.postgresql");
      FilePath outFile3Sqlite = workingDir.completeChildPath("3_AddAccountCreationTime.sqlite");
      FilePath outFile3Postgresql = workingDir.completeChildPath("3_AddAccountCreationTime.postgresql");

      REQUIRE_FALSE(writeStringToFile(outFile1, schema1));
      REQUIRE_FALSE(writeStringToFile(outFile2Sqlite, schema2Sqlite));
      REQUIRE_FALSE(writeStringToFile(outFile2Postgresql, schema2Postgresql));
      REQUIRE_FALSE(writeStringToFile(outFile3Sqlite, schema3Sqlite));
      REQUIRE_FALSE(writeStringToFile(outFile3Postgresql, schema3Postgresql));

      boost::shared_ptr<Connection> sqliteConnection;
      REQUIRE_FALSE(connect(sqliteConnectionOptions(), &sqliteConnection));

      boost::shared_ptr<Connection> postgresConnection;
      REQUIRE_FALSE(connect(postgresConnectionOptions(), &postgresConnection));

      SchemaUpdater sqliteUpdater(sqliteConnection, workingDir);
      SchemaUpdater postgresUpdater(postgresConnection, workingDir);

      REQUIRE_FALSE(sqliteUpdater.update());
      REQUIRE_FALSE(postgresUpdater.update());

      std::string currentSchemaVersion;
      REQUIRE_FALSE(sqliteUpdater.databaseSchemaVersion(&currentSchemaVersion));
      REQUIRE(currentSchemaVersion == "3_AddAccountCreationTime");
      currentSchemaVersion.clear();
      REQUIRE_FALSE(postgresUpdater.databaseSchemaVersion(&currentSchemaVersion));
      REQUIRE(currentSchemaVersion == "3_AddAccountCreationTime");

      // ensure repeated calls to update work without error
      REQUIRE_FALSE(sqliteUpdater.update());
      REQUIRE_FALSE(postgresUpdater.update());

      // ensure we can insert data as expected (given our expected constraints)
      int id = 1;
      std::string firstName = "Billy";
      std::string lastName = "Joel";
      std::string email = "bjoel@example.com";
      std::string creationTime = "03/03/2020 12:00:00";

      // create queries - we will be executing them multiple times, so bind input just before execution
      Query sqliteInsertQuery = sqliteConnection->query("INSERT INTO TestTable1_Persons VALUES (:id, :fname, :lname, :email)");
      Query postgresInsertQuery = postgresConnection->query("INSERT INTO TestTable1_Persons VALUES (:id, :fname, :lname, :email)");
      Query sqliteInsertQuery2 = sqliteConnection->query("INSERT INTO TestTable2_AccountHolders VALUES (:id, :pid, :time)");
      Query postgresInsertQuery2 = postgresConnection->query("INSERT INTO TestTable2_AccountHolders VALUES (:id, :pid, :time)");

      // should fail - FK constraint
      sqliteInsertQuery2
            .withInput(id, "id")
            .withInput(id, "pid")
            .withInput(creationTime, "time");
      postgresInsertQuery2
            .withInput(id, "id")
            .withInput(id, "pid")
            .withInput(creationTime, "time");
      CHECK(sqliteConnection->execute(sqliteInsertQuery2));
      CHECK(postgresConnection->execute(postgresInsertQuery2));

      // should succeed - properly ordered
      sqliteInsertQuery
            .withInput(id, "id")
            .withInput(firstName, "fname")
            .withInput(lastName, "lname")
            .withInput(email, "email");
      CHECK_FALSE(sqliteConnection->execute(sqliteInsertQuery));
      sqliteInsertQuery2
            .withInput(id, "id")
            .withInput(id, "pid")
            .withInput(creationTime, "time");
      CHECK_FALSE(sqliteConnection->execute(sqliteInsertQuery2));
      postgresInsertQuery
            .withInput(id, "id")
            .withInput(firstName, "fname")
            .withInput(lastName, "lname")
            .withInput(email, "email");
      CHECK_FALSE(postgresConnection->execute(postgresInsertQuery));
      postgresInsertQuery2
            .withInput(id, "id")
            .withInput(id, "pid")
            .withInput(creationTime, "time");
      CHECK_FALSE(postgresConnection->execute(postgresInsertQuery2));

      // should fail - PK constraint
      sqliteInsertQuery
            .withInput(id, "id")
            .withInput(firstName, "fname")
            .withInput(lastName, "lname")
            .withInput(email, "email");
      CHECK(sqliteConnection->execute(sqliteInsertQuery));
      sqliteInsertQuery2
            .withInput(id, "id")
            .withInput(id, "pid")
            .withInput(creationTime, "time");
      CHECK(sqliteConnection->execute(sqliteInsertQuery2));
      postgresInsertQuery
            .withInput(id, "id")
            .withInput(firstName, "fname")
            .withInput(lastName, "lname")
            .withInput(email, "email");
      CHECK(postgresConnection->execute(postgresInsertQuery));
      postgresInsertQuery2
            .withInput(id, "id")
            .withInput(id, "pid")
            .withInput(creationTime, "time");
      CHECK(postgresConnection->execute(postgresInsertQuery2));
   }
}

} // namespace unit_tests
} // namespace rstudio