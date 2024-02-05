/*
 * FTP Serveur for ESP8266
 * based on FTP Serveur for Arduino Due and Ethernet shield (W5100) or WIZ820io (W5200)
 * based on Jean-Michel Gallego's work
 * modified to work with esp8266 SPIFFS by David Paiva david@nailbuster.com
 * modified to make it work by Sha
 * modified to make it work with LittleFS by Sha
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "FtpServer.h"

#ifdef ESP8266
#include <ESP8266WiFi.h>
#elif defined ESP32
#include <WiFi.h>
#endif

//#warning fichier EspFtpServer.h
WiFiServer ftpServer(FTP_CTRL_PORT);
WiFiServer dataServer(FTP_DATA_PORT_PASV);

void FtpServer::begin(String uname, String pword)
{
  // Tells the ftp server to begin listening for incoming connection
  _FTP_USER = uname;
  _FTP_PASS = pword;

  ftpServer.begin();
  delay(10);
  dataServer.begin();
  delay(10);
  millisTimeOut = (uint32_t)FTP_TIME_OUT * 60 * 1000;
  millisDelay = 0;
  cmdStatus = cInit;
  iniVariables();
  FTPdebug("Initialisation du serveur FTP\n");
}

void FtpServer::iniVariables()
{
  // Default for data port
  dataPort = FTP_DATA_PORT_PASV;

  // Default Data connection is Active
  dataPassiveConn = true;

  // Set the root directory
  strcpy(cwdName, "/");

  rnfrCmd = false;
  transferStatus = 0;
}

boolean FtpServer::handleFTP()
{

 // if ((int32_t)(millisDelay - millis()) > 0)   return transfer_en_cours;

  transfer_en_cours = false;
  if (ftpServer.hasClient())
  {
    client.stop();
    client = ftpServer.accept();
  }

  if (cmdStatus == cInit)
  {
    if (client.connected())
      disconnectClient();
    cmdStatus = cWait;
  }
  else if (cmdStatus == cWait) // Ftp server waiting for connection
  {
    abortTransfer();
    iniVariables();

    FTPdebug("FTP server en attente de connexion sur le port %d\n", FTP_CTRL_PORT);

    cmdStatus = cCheck;
  }
  else if (cmdStatus == cCheck) // Ftp server idle
  {
    if (client.connected()) // A client connected
    {
      clientConnected();
      millisEndConnection = millis() + 10 * 1000; // wait client id during 10 s.
      cmdStatus = cUserId;
    }
  }
  else if (readChar() > 0) // got response
  {
    if (cmdStatus == cUserId) // Ftp server waiting for user identity
      if (userIdentity())
      {
        cmdStatus = cPassword;
      }
      else
      {
        cmdStatus = cInit;
      }
    else if (cmdStatus == cPassword) // Ftp server waiting for user registration
      if (userPassword())
      {
        cmdStatus = cLoginOk;
        millisEndConnection = millis() + millisTimeOut;
      }
      else
      {
        cmdStatus = cInit;
      }
    else if ((cmdStatus == cLoginOk)) // Ftp server waiting for user command
    {
      if (!processCommand())
      {
        cmdStatus = cInit;
      }
      else
      {
        millisEndConnection = millis() + millisTimeOut;
      }
    }
  }
  else if (!client.connected() || !client)
  {
    cmdStatus = cWait;
    FTPdebug("client disconnected\n");
  }

  if (transferStatus == 1) // Retrieve data
  {
    if (!doRetrieve())
    {
      transferStatus = 0;
    }
    else
    {
      transfer_en_cours = true;
    }
  }
  else if (transferStatus == 2) // Store data
  {
    if (!doStore())
    {
      transferStatus = 0;
    }
    else
    {
      transfer_en_cours = true;
    }
  }
  else if (cmdStatus > 2 && !((int32_t)(millisEndConnection - millis()) > 0))
  {
    client.println("530 Timeout");
    millisDelay = millis() + 200; // delay of 200 ms
    cmdStatus = cInit;
  }
  return transfer_en_cours;
}

void FtpServer::clientConnected()
{
  FTPdebug("Client connected!\n");

  client.println("220 --- Welcome to FTP for ESP8266/ESP32 ---");
  client.println("220 --- By le Sha ---");
  client.println("220 --- Version " + String(FTP_SERVER_VERSION) + " ---");
  iCL = 0;
}

void FtpServer::disconnectClient()
{
  FTPdebug("Disconnecting client\n");

  abortTransfer();
  client.println("221 Goodbye");
  client.stop();
}

boolean FtpServer::userIdentity()
{
  FTPdebug("cmnd = %s %s\n", command, parameters);

  if (strcmp(command, "USER"))
  {
    client.println("500 Syntax error");
    FTPdebug("500 commande USER attendue\n");
  }
  else
  {
    if (strcmp(parameters, _FTP_USER.c_str()))
    {
      client.println("530 user not found");
      FTPdebug("530 pas le USER attendu : %s\n", _FTP_USER.c_str());
    }
    else
    {
      client.println("331 OK. Password required");
      FTPdebug("331 on attend le password\n");
      strcpy(cwdName, "/");
      return true;
    }
  }
  millisDelay = millis() + 100; // delay of 100 ms
  return false;
}

boolean FtpServer::userPassword()
{
  FTPdebug("cmnd = %s %s\n", command, parameters);

  if (strcmp(command, "PASS"))
  {
    client.println("500 Syntax error");
    FTPdebug("500 commande PASS attendue\n");
  }
  else if (strcmp(parameters, _FTP_PASS.c_str()))
  {
    client.println("530 ");
  }
  else
  {
    FTPdebug("Password OK. En attente de commandes.\n");
    client.println("230 OK.");
    return true;
  }
  millisDelay = millis() + 100; // delay of 100 ms
  return false;
}

boolean FtpServer::processCommand()
{
  ///////////////////////////////////////
  //                                   //
  //      ACCESS CONTROL COMMANDS      //
  //                                   //
  ///////////////////////////////////////

  //
  //  CDUP - Change to Parent Directory
  //
  if (!strcmp(command, "CDUP"))
  {
    FTPdebug("cmnd = %s\n", command);
    client.println("250 Ok. Current directory is " + String(cwdName));
  }
  //
  //  CWD - Change Working Directory
  //
  else if (!strcmp(command, "CWD"))
  {
    //char path[FTP_CWD_SIZE];
    FTPdebug("cmnd = %s\n", command);
    if (strcmp(parameters, ".") == 0) // 'CWD .' is the same as PWD command
      client.println("257 \"" + String(cwdName) + "\" is your current directory");
    else
    {
      client.println("250 Ok. Current directory is " + String(cwdName));
    }
  }
  //
  //  PWD - Print Directory
  //
  else if (!strcmp(command, "PWD"))
  {
    FTPdebug("cmnd = %s\n", command);
    client.println("257 \"" + String(cwdName) + "\" is your current directory");
  }
  //
  //  QUIT
  //
  else if (!strcmp(command, "QUIT"))
  {
    FTPdebug("cmnd = %s\n", command);
    disconnectClient();
    return false;
  }

  ///////////////////////////////////////
  //                                   //
  //    TRANSFER PARAMETER COMMANDS    //
  //                                   //
  ///////////////////////////////////////

  //
  //  MODE - Transfer Mode
  //
  else if (!strcmp(command, "MODE"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    if (!strcmp(parameters, "S"))
      client.println("200 S Ok");
    // else if( ! strcmp( parameters, "B" ))
    //  client.println( "200 B Ok\r\n";
    else
      client.println("504 Only S(tream) is suported");
  }
  //
  //  PASV - Passive Connection management
  //
  else if (!strcmp(command, "PASV"))
  {
    FTPdebug("cmnd = %s\n", command);
    if (data.connected())
    {
      data.stop();
    }
    //dataServer.begin();
    //dataIp = Ethernet.localIP();
    dataIp = client.localIP();
    dataPort = FTP_DATA_PORT_PASV;
    //data.connect( dataIp, dataPort );
    //data = dataServer.available();

    FTPdebug("Connection management set to passive\n");
    FTPdebug("Data port set to %d\n", dataPort);

    client.println("227 Entering Passive Mode (" + String(dataIp[0]) + "," + String(dataIp[1]) + "," + String(dataIp[2]) + "," + String(dataIp[3]) + "," + String(dataPort >> 8) + "," + String(dataPort & 255) + ").");
    dataPassiveConn = true;
  }
  //
  //  PORT - Data Port
  //
  else if (!strcmp(command, "PORT"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    if (data)
      data.stop();
    // get IP of data client
    dataIp[0] = atoi(parameters);
    char *p = strchr(parameters, ',');
    for (uint8_t i = 1; i < 4; i++)
    {
      dataIp[i] = atoi(++p);
      p = strchr(p, ',');
    }
    // get port of data client
    dataPort = 256 * atoi(++p);
    p = strchr(p, ',');
    dataPort += atoi(++p);
    if (p == NULL)
      client.println("501 Can't interpret parameters");
    else
    {
      client.println("200 PORT command successful");
      dataPassiveConn = false;
    }
  }
  //
  //  STRU - File Structure
  //
  else if (!strcmp(command, "STRU"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    if (!strcmp(parameters, "F"))
      client.println("200 F Ok");
    // else if( ! strcmp( parameters, "R" ))
    //  client.println( "200 B Ok\r\n";
    else
      client.println("504 Only F(ile) is suported");
  }
  //
  //  TYPE - Data Type
  //
  else if (!strcmp(command, "TYPE"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    if (!strcmp(parameters, "A"))
      client.println("200 TYPE is now ASII");
    else if (!strcmp(parameters, "I"))
      client.println("200 TYPE is now 8-bit binary");
    else
      client.println("504 Unknow TYPE");
  }

  ///////////////////////////////////////
  //                                   //
  //        FTP SERVICE COMMANDS       //
  //                                   //
  ///////////////////////////////////////

  //
  //  ABOR - Abort
  //
  else if (!strcmp(command, "ABOR"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    abortTransfer();
    client.println("226 Data connection closed");
  }
  //
  //  DELE - Delete a File
  //
  else if (!strcmp(command, "DELE"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    char path[FTP_CWD_SIZE];
    if (strlen(parameters) == 0)
      client.println("501 No file name");
    else if (makePath(path))
    {
      if (!FTP_FS.exists(path))
        client.println("550 File " + String(parameters) + " not found");
      else
      {
        if (FTP_FS.remove(path))
        {
          FTPdebug("Fichier supprimé %s\n", parameters);
          client.println("250 Deleted " + String(parameters));
        }
        else
          client.println("450 Can't delete " + String(parameters));
      }
    }
  }
  //
  //  LIST - List
  //
  else if (!strcmp(command, "LIST"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    if (!dataConnect())
      client.println("425 No data connection");
    else
    {
      client.println("150 Accepted data connection");
      uint16_t nm = 0;
#ifdef ESP8266
      Dir dir = FTP_FS.openDir(cwdName);
      // if( !FTP_FS.exists(cwdName))
      //   client.println( "550 Can't open directory " + String(cwdName) );
      // else
      {
        while (dir.next())
        {
          String fn, fs;
          fn = dir.fileName();
          //fn.remove(0, 1);       chgt suite au passage en littleFS
          fs = String(dir.fileSize());
          data.println("+r,s" + fs);
          data.println(",\t" + fn);
          nm++;
        }
        client.println("226 " + String(nm) + " matches total");
      }
#elif defined ESP32
      File root = FTP_FS.open(cwdName);
      if (!root)
      {
        client.println("550 Can't open directory " + String(cwdName));
        // return;
      }
      else
      {
        // if(!root.isDirectory()){
        // 		Serial.println("Not a directory");
        // 		return;
        // }

        File file = root.openNextFile();
        while (file)
        {
          if (file.isDirectory())
          {
            data.println("+r,s <DIR> " + String(file.name()));
            // Serial.print("  DIR : ");
            // Serial.println(file.name());
            // if(levels){
            // 	listDir(fs, file.name(), levels -1);
            // }
          }
          else
          {
            String fn, fs;
            fn = file.name();
            // fn.remove(0, 1);
            fs = String(file.size());
            data.println("+r,s" + fs);
            data.println(",\t" + fn);
            nm++;
          }
          file = root.openNextFile();
        }
        client.println("226 " + String(nm) + " matches total");
      }
#endif
      data.stop();
    }
  }
  //
  //  MLSD - Listing for Machine Processing (see RFC 3659)
  //
  else if (!strcmp(command, "MLSD"))
  {
    FTPdebug("cmnd = %s\n", command);
    if (!dataConnect())
      client.println("425 No data connection MLSD");
    else
    {
      client.println("150 Accepted data connection");
      uint16_t nm = 0;
#ifdef ESP8266
      Dir dir = FTP_FS.openDir(cwdName);
      //char dtStr[15];
      //  if(!FTP_FS.exists(cwdName))
      //    client.println( "550 Can't open directory " +String(parameters)+ );
      //  else
      {
        while (dir.next())
        {
          String fn, fs;
          time_t fct;
          tm tm_locale;
          char strftime_buf[15];
          fn = dir.fileName();
          // FTPdebug("file = %s\n", (char*)fn.c_str());
          //fn.remove(0, 1);          chgt suite au passage en littleFS
          FTPdebug("file = %s\n", (char*)fn.c_str());
          fs = String(dir.fileSize());
          fct = dir.fileCreationTime();
          localtime_r(&fct, &tm_locale);
          strftime(strftime_buf, sizeof(strftime_buf), "%Y%m%d%H%M%S", &tm_locale);
          data.println("Type=file;Size=" + fs + ";modify=" + "20230515160656" + ";" + fn);
          nm++;
        }
        client.println("226-options: -a -l");
        client.println("226 " + String(nm) + " matches total");
      }
#elif defined ESP32
      File root = FTP_FS.open(cwdName);
      // if(!root){
      // 		client.println( "550 Can't open directory " + String(cwdName) );
      // 		// return;
      // } else {
      // if(!root.isDirectory()){
      // 		Serial.println("Not a directory");
      // 		return;
      // }

      File file = root.openNextFile();
      while (file)
      {
        // if(file.isDirectory()){
        // 	data.println( "+r,s <DIR> " + String(file.name()));
        // 	// Serial.print("  DIR : ");
        // 	// Serial.println(file.name());
        // 	// if(levels){
        // 	// 	listDir(fs, file.name(), levels -1);
        // 	// }
        // } else {
        String fn, fs;
        fn = file.name();
        fn.remove(0, 1);
        fs = String(file.size());
        data.println("Type=file;Size=" + fs + ";" + "modify=20000101160656;" + " " + fn);
        nm++;
        // }
        file = root.openNextFile();
      }
      client.println("226-options: -a -l");
      client.println("226 " + String(nm) + " matches total");
      // }
#endif
      data.stop();
    }
  }
  //
  //  NLST - Name List
  //
  else if (!strcmp(command, "NLST"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    if (!dataConnect())
      client.println("425 No data connection");
    else
    {
      client.println("150 Accepted data connection");
      uint16_t nm = 0;
#ifdef ESP8266
      Dir dir = FTP_FS.openDir(cwdName);
      // if( !FTP_FS.exists( cwdName ))
      //   client.println( "550 Can't open directory " + String(parameters));
      // else
      {
        while (dir.next())
        {
          data.println(dir.fileName());
          nm++;
        }
        client.println("226 " + String(nm) + " matches total");
      }
#elif defined ESP32
      File root = FTP_FS.open(cwdName);
      if (!root)
      {
        client.println("550 Can't open directory " + String(cwdName));
      }
      else
      {

        File file = root.openNextFile();
        while (file)
        {
          data.println(file.name());
          nm++;
          file = root.openNextFile();
        }
        client.println("226 " + String(nm) + " matches total");
      }
#endif
      data.stop();
    }
  }
  //
  //  NOOP
  //
  else if (!strcmp(command, "NOOP"))
  {
    FTPdebug("cmnd = %s\n", command);
    // dataPort = 0;
    client.println("200 Zzz...");
  }
  //
  //  RETR - Retrieve
  //
  else if (!strcmp(command, "RETR"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    char path[FTP_CWD_SIZE];
    if (strlen(parameters) == 0)
    {
      client.println("501 No file name");
    }
    else if (makePath(path))
    {
      file = FTP_FS.open(path, "r");
      if (!file)
      {
        client.println("550 File " + String(parameters) + " not found");
        client.println("450 Can't open " + String(parameters));
      }
      else if (!dataConnect())
      {
        client.println("425 No data connection");
        file.close();
      }
      else
      {
        FTPdebug("Sending %s\n", parameters);

        client.println("150-Connected to port " + String(dataPort));
        client.println("150 " + String(file.size()) + " bytes to download");
        millisBeginTrans = millis();
        bytesTransfered = 0;
        transferStatus = 1;
      }
    }
  }
  //
  //  STOR - Store
  //
  else if (!strcmp(command, "STOR"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);

    char path[FTP_CWD_SIZE];
    if (strlen(parameters) == 0)
    {
      client.println("501 No file name");
    }
    else if (makePath(path))
    {
      FTPdebug("path = %s\n", path);
      file = FTP_FS.open(path, "w");
      if (!file)
      {
        client.println("451 Can't open/create " + String(parameters));
      }
      else if (!dataConnect())
      {
        client.println("425 No data connection");
        FTPdebug("425 Pas de connexion\n");
        file.close();
      }
      else
      {
        FTPdebug("Receiving %s\n", parameters);

        client.println("150 Connected to port " + String(dataPort));
        millisBeginTrans = millis();
        bytesTransfered = 0;
        transferStatus = 2;
      }
    }
  }
  //
  //  MKD - Make Directory
  //
  else if (!strcmp(command, "MKD"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    client.println("550 Can't create \"" + String(parameters)); // pas encore de support
  }
  //
  //  RMD - Remove a Directory
  //
  else if (!strcmp(command, "RMD"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    client.println("501 Can't delete \"" + String(parameters));
  }
  //
  //  RNFR - Rename From
  //
  else if (!strcmp(command, "RNFR"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    buf[0] = 0;
    if (strlen(parameters) == 0)
      client.println("501 No file name");
    else if (makePath(buf))
    {
      if (!FTP_FS.exists(buf))
        client.println("550 File " + String(parameters) + " not found");
      else
      {
        FTPdebug("Renaming %s\n", buf);

        client.println("350 RNFR accepted - file exists, ready for destination");
        rnfrCmd = true;
      }
    }
  }
  //
  //  RNTO - Rename To
  //
  else if (!strcmp(command, "RNTO"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    char path[FTP_CWD_SIZE];
    //char dir[FTP_FIL_SIZE];
    if (strlen(buf) == 0 || !rnfrCmd)
      client.println("503 Need RNFR before RNTO");
    else if (strlen(parameters) == 0)
      client.println("501 No file name");
    else if (makePath(path))
    {
      if (FTP_FS.exists(path))
        client.println("553 " + String(parameters) + " already exists");
      else
      {
        FTPdebug("Renaming %s to %s\n", buf, path);

        if (FTP_FS.rename(buf, path))
          client.println("250 File successfully renamed or moved");
        else
          client.println("451 Rename/move failure");
      }
    }
    rnfrCmd = false;
  }

  ///////////////////////////////////////
  //                                   //
  //   EXTENSIONS COMMANDS (RFC 3659)  //
  //                                   //
  ///////////////////////////////////////

  //
  //  FEAT - New Features
  //
  else if (!strcmp(command, "FEAT"))
  {
    FTPdebug("cmnd = %s \n", command);
    client.println("211-Extensions suported:");
    client.println(" MLSD");
    client.println("211 End.");
  }
  //
  //  MDTM - File Modification Time (see RFC 3659)
  //
  else if (!strcmp(command, "MDTM"))
  {
    FTPdebug("cmnd = %s\n", command);
    client.println("550 Unable to retrieve time");
  }

  //
  //  SIZE - Size of the file
  //
  else if (!strcmp(command, "SIZE"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    char path[FTP_CWD_SIZE];
    if (strlen(parameters) == 0)
      client.println("501 No file name");
    else if (makePath(path))
    {
      file = FTP_FS.open(path, "r");
      if (!file)
        client.println("450 Can't open " + String(parameters));
      else
      {
        client.println("213 " + String(file.size()));
        file.close();
      }
    }
  }
  //
  //  SITE - System command
  //
  else if (!strcmp(command, "SITE"))
  {
    FTPdebug("cmnd = %s %s\n", command, parameters);
    client.println("500 Unknow SITE command " + String(parameters));
  }
  //
  //  Unrecognized commands ...
  //
  else
    client.println("500 Unknow command");

  return true;
}

boolean FtpServer::dataConnect()
{
  unsigned long startTime = millis();
  //wait 5 seconds for a data connection
  if (!data.connected())
  {
    FTPdebug("data non connecté\n");
    while (!dataServer.hasClient() && (millis() - startTime < 5000))
    {
      yield();
    }
    if (dataServer.hasClient())
    {
      FTPdebug("ftpdataserver client.... %dms\n", millis() - startTime);
      data.stop();
      data = dataServer.available();
      return true;   // osé ?? !!
    }
    else
    {
      FTPdebug("time out après 5s\n");
      return false;
    }
    //return data.connected();
  }
  else
  {
    FTPdebug("TRUE\n");
    return true;
  }
}

boolean FtpServer::doRetrieve()
{
  if (data.connected())
  {
    int16_t nb = file.readBytes(buf, FTP_BUF_SIZE);
    if (nb > 0)
    {
      FTPdebug("data envoyées %d\n", nb);
      data.write((uint8_t *)buf, nb);
      bytesTransfered += nb;
      return true;
    }
  }
  closeTransfer();   // pas de connexion ou fin du fichier
  return false;
}

boolean FtpServer::doStore()
{
  // Avoid blocking by never reading more bytes than are available
  int navail = data.available();
  FTPdebug("data disponibles %d\n", navail);
  if (navail > 0)
  {
    //FTPdebug("data disponibles %d\n", navail);
    // And be sure not to overflow buf.
    if (navail > FTP_BUF_SIZE)
    {
      navail = FTP_BUF_SIZE;
    }
    int16_t nb = data.read((uint8_t *)buf, navail);
    FTPdebug("data lues %d\n", nb);
    // int16_t nb = data.readBytes((uint8_t*) buf, FTP_BUF_SIZE );
    if (nb > 0)
    {
      // Serial.println( millis() << " " << nb << endl;
      file.write((uint8_t *)buf, nb);
      FTPdebug("data ecrites %d\n", nb);
      bytesTransfered += nb;
    }
  }
  if (!data.connected() && (navail <= 0) && (millis() - millisBeginTrans > 100))
  {
    FTPdebug("fermeture du transfert\n");
    closeTransfer();
    return false;
  }
  else
  {
    FTPdebug("transfert en cours\n");
    return true;
  }
}

void FtpServer::closeTransfer()
{
  uint32_t deltaT = (int32_t)(millis() - millisBeginTrans);
  if (deltaT > 0 && bytesTransfered > 0)
  {
    client.println("226-File successfully transferred");
    client.println("226 " + String(deltaT) + " ms, " + String(bytesTransfered / deltaT) + " kbytes/s");
    FTPdebug("Transfert terminé : %d bytes transférés\n", bytesTransfered);
  }
  else
  {
    FTPdebug("Transfert terminé avec succès\n");
    client.println("226 File successfully transferred");
  }

  file.close();
  data.stop();
}

void FtpServer::abortTransfer()
{
  if (transferStatus > 0)
  {
    file.close();
    data.stop();
    client.println("426 Transfer aborted");
    FTPdebug("Transfert avorté\n");
  }
  transferStatus = 0;
}

// Read a char from client connected to ftp server
//
//  update cmdLine and command buffers, iCL and parameters pointers
//
//  return:
//    -2 if buffer cmdLine is full
//    -1 if line not completed
//     0 if empty line received
//    length of cmdLine (positive) if no empty line received

int8_t FtpServer::readChar()
{
  int8_t rc = -1;

  if (client.available())
  {
    char c = client.read();
    // char c;
    // client.readBytes((uint8_t*) c, 1);
#ifdef FTP_DEBUG
    Serial.print(c);
#endif
    if (c == '\\')
    {
      c = '/';
    }

    if ((c != '\r'))
    {
      if ((c != '\n'))
      {
        if (iCL < FTP_CMD_SIZE)
        {
          cmdLine[iCL++] = c;
        }
        else
        {
          rc = -2; //  Line too long
        }
      }
      else
      {
        cmdLine[iCL] = 0;
        command[0] = 0;
        parameters = NULL;
        // empty line?
        if (iCL == 0)
        {
          rc = 0;
        }
        else
        {
          rc = iCL;
          // search for space between command and parameters
          parameters = strchr(cmdLine, ' ');
          if (parameters != NULL)
          {
            if (parameters - cmdLine > 4)
            {
              rc = -2; // Syntax error
            }
            else
            {
              strncpy(command, cmdLine, parameters - cmdLine);
              command[parameters - cmdLine] = 0;

              while (*(++parameters) == ' ')
                ;
            }
          }
          else if (strlen(cmdLine) > 4)
          {
            rc = -2; // Syntax error.
          }
          else
            strcpy(command, cmdLine);
          iCL = 0;
        }
      }
    }
    if (rc > 0)
    {
      for (uint8_t i = 0; i < strlen(command); i++)
      {
        command[i] = toupper(command[i]);
      }
    }
    if (rc == -2)
    {
      iCL = 0;
      client.println("500 Syntax error");
    }
  }
  return rc;
}

// Make complete path/name from cwdName and parameters
//
// 3 possible cases: parameters can be absolute path, relative path or only the name
//
// parameters:
//   fullName : where to store the path/name
//
// return:
//    true, if done

boolean FtpServer::makePath(char *fullName)
{
  return makePath(fullName, parameters);
}

boolean FtpServer::makePath(char *fullName, char *param)
{
  if (param == NULL)
    param = parameters;

  // Root or empty?
  if (strcmp(param, "/") == 0 || strlen(param) == 0)
  {
    strcpy(fullName, "/");
    return true;
  }
  // If relative path, concatenate with current dir
  if (param[0] != '/')
  {
    strcpy(fullName, cwdName);
    if (fullName[strlen(fullName) - 1] != '/')
      strncat(fullName, "/", FTP_CWD_SIZE);
    strncat(fullName, param, FTP_CWD_SIZE);
  }
  else
    strcpy(fullName, param);
  // If ends with '/', remove it
  uint16_t strl = strlen(fullName) - 1;
  if (fullName[strl] == '/' && strl > 1)
    fullName[strl] = 0;
  if (strlen(fullName) < FTP_CWD_SIZE)
    return true;

  client.println("500 Command line too long");
  return false;
}

/*
Calculate year, month, day, hour, minute and second
  from first parameter sent by MDTM command (YYYYMMDDHHMMSS)

parameters:
  pyear, pmonth, pday, phour, pminute and psecond: pointer of
    variables where to store data

return:
   0 if parameter is not YYYYMMDDHHMMSS
   length of parameter + space
*/

/* 
uint8_t FtpServer::getDateTime(uint16_t *pyear, uint8_t *pmonth, uint8_t *pday,
                               uint8_t *phour, uint8_t *pminute, uint8_t *psecond)
{
  char dt[15];

  // Date/time are expressed as a 14 digits long string
  //   terminated by a space and followed by name of file
  if (strlen(parameters) < 15 || parameters[14] != ' ')
    return 0;
  for (uint8_t i = 0; i < 14; i++)
    if (!isdigit(parameters[i]))
      return 0;

  strncpy(dt, parameters, 14);
  dt[14] = 0;
  *psecond = atoi(dt + 12);
  dt[12] = 0;
  *pminute = atoi(dt + 10);
  dt[10] = 0;
  *phour = atoi(dt + 8);
  dt[8] = 0;
  *pday = atoi(dt + 6);
  dt[6] = 0;
  *pmonth = atoi(dt + 4);
  dt[4] = 0;
  *pyear = atoi(dt);
  return 15;
}
*/

/*
Create string YYYYMMDDHHMMSS from date and time

parameters:
   date, time
   tstr: where to store the string. Must be at least 15 characters long

return:
   pointer to tstr
*/

/* 
char *FtpServer::makeDateTimeStr(char *tstr, uint16_t date, uint16_t time)
{
  sprintf(tstr, "%04u%02u%02u%02u%02u%02u",
          ((date & 0xFE00) >> 9) + 1980, (date & 0x01E0) >> 5, date & 0x001F,
          (time & 0xF800) >> 11, (time & 0x07E0) >> 5, (time & 0x001F) << 1);
  return tstr;
} 
*/
