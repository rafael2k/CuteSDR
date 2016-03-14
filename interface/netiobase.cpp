//////////////////////////////////////////////////////////////////////
// NetInterface.cpp: implementation of the CNetio and CUdp.
//
//  This class implements the low level UDP and TCP interfaces.
//
// History:
//	2012-12-12  Initial creation MSW
//	2013-02-05  Modified for CuteSDR. Now TCP runs in gui thread to simplify
//	2013-07-28  fixed DisconnectFromServerSlot bug
//	2015-03-26  Added  support for small MTU and UDP keepalive in case of port forwarding timeouts
//	2015-07-13  removed windsock dependency, Changed a few threading issues
/////////////////////////////////////////////////////////////////////
//==========================================================================================
// + + +   This Software is released under the "Simplified BSD License"  + + +
//Copyright 2010 Moe Wheatley. All rights reserved.
//
//Redistribution and use in source and binary forms, with or without modification, are
//permitted provided that the following conditions are met:
//
//   1. Redistributions of source code must retain the above copyright notice, this list of
//	  conditions and the following disclaimer.
//
//   2. Redistributions in binary form must reproduce the above copyright notice, this list
//	  of conditions and the following disclaimer in the documentation and/or other materials
//	  provided with the distribution.
//
//THIS SOFTWARE IS PROVIDED BY Moe Wheatley ``AS IS'' AND ANY EXPRESS OR IMPLIED
//WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
//FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL Moe Wheatley OR
//CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
//SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
//ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
//ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//The views and conclusions contained in the software and documentation are those of the
//authors and should not be interpreted as representing official policies, either expressed
//or implied, of Moe Wheatley.
//=============================================================================
/*---------------------------------------------------------------------------*/
/*------------------------> I N C L U D E S <--------------------------------*/
/*---------------------------------------------------------------------------*/
#include <QtNetwork>
#include <QDebug>
#include "netiobase.h"

#define MSGSTATE_HDR1 0		//ASCP msg assembly states
#define MSGSTATE_HDR2 1
#define MSGSTATE_DATA 2

#define RCVBUF_SIZE_UDP 2097152

//*********    C U d p  I m p l e m e n t a t i o n       ***********
/////////////////////////////////////////////////////////////////////
// Constructor/Destructor
/////////////////////////////////////////////////////////////////////
CUdp::CUdp(QObject *parent) : m_pParent(parent)
{
qDebug()<<"CUdp constructor";
}

CUdp::~CUdp()
{
	CleanupThread();	//tell thread to cleanup after itself by calling ThreadExit()
qDebug()<<"CUdp destructor";
}

////////////////////////////////////////////////////////////////////////
//  called by CUdp worker thread to initialize its world
////////////////////////////////////////////////////////////////////////
void CUdp::ThreadInit()	//override called by new thread when started
{
	m_pUdpSocket = new QUdpSocket;
	m_pUdpFwdSocket = new QUdpSocket;
	connect(m_pParent, SIGNAL(StartUdp(quint32, quint32, quint16) ), this, SLOT( StartUdpSlot(quint32, quint32, quint16) ) );
	connect(m_pParent, SIGNAL(StopUdp() ), this, SLOT( StopUdpSlot() ) );
	connect(m_pParent, SIGNAL(SendUdpKeepalive() ), this, SLOT( SendUdpKeepaliveSlot() ) );
qDebug()<<"UDP Thread "<<this->thread()->currentThread();
}

/////////////////////////////////////////////////////////////////////
// Called by this worker thread to cleanup after itself
/////////////////////////////////////////////////////////////////////
void CUdp::ThreadExit()
{
	disconnect();
	if(m_pUdpSocket)
		delete m_pUdpSocket;
	if(m_pUdpFwdSocket)
		delete m_pUdpFwdSocket;
}

////////////////////////////////////////////////////////////////////////
// Binds and starts up UDP receive
////////////////////////////////////////////////////////////////////////
void CUdp::StartUdpSlot(quint32 ServerAdr, quint32 ClientAdr, quint16 ServerPort)
{
QHostAddress CIPAdr(ClientAdr);
	m_ServerIPAdr.setAddress(ServerAdr);
	m_ServerPort = ServerPort;

	m_pThread->setPriority(QThread::HighestPriority);
	if(m_pUdpSocket->bind( ServerPort ) )
	{
		//need to bump socket memory buffer size for higher speeds
		m_pUdpSocket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, RCVBUF_SIZE_UDP);
		connect(m_pUdpSocket, SIGNAL(readyRead()), this, SLOT(GotUdpData()));
		qDebug()<<"Udp Bind ok"<<CIPAdr<< ServerPort;
	}
	else
	{
		qDebug()<<"Udp Bind fail"<<CIPAdr << ServerPort<<m_pUdpSocket->errorString().toLocal8Bit();
	}
	m_pUdpFwdSocket->bind( m_FwdPort );
}

////////////////////////////////////////////////////////////////////////
// Stops and closes UDP socket
////////////////////////////////////////////////////////////////////////
void CUdp::StopUdpSlot()
{
	qDebug()<<"Stop Udp";
	if(m_pUdpSocket->isValid())
	{
		m_pUdpSocket->flush();
		m_pUdpSocket->close();
	}
}

////////////////////////////////////////////////////////////////////////
// Called via connect from UDP thread to process new rx data
////////////////////////////////////////////////////////////////////////
void CUdp::GotUdpData()
{
char pBuf[20000];
	//loop while there are still datagrams to get
	while( m_pUdpSocket->hasPendingDatagrams() )
	{
		qint64 n = m_pUdpSocket->pendingDatagramSize();
		if(n<20000)
		{
			m_pUdpSocket->readDatagram(pBuf, n);
			((CNetio*)m_pParent)->ProcessUdpData(pBuf, n);
			if(m_UseUdpFwd)
				if( m_pUdpFwdSocket->isValid())
					m_pUdpFwdSocket->writeDatagram(pBuf, n, m_IPFwdAdr, m_FwdPort);
#if 0
static unsigned char seq=0;
if(seq!=(unsigned char)pBuf[3])
{
	seq = (unsigned char)pBuf[3];
	qDebug("%X %u",(unsigned char)pBuf[3], (unsigned int)n);
}
#endif
		}
	}
}

////////////////////////////////////////////////////////////////////////
// slot Called send UDP data keepalive
////////////////////////////////////////////////////////////////////////
void CUdp::SendUdpKeepaliveSlot()
{
char DummyData = 0x5A;
	if( m_pThread->isRunning() )
	{
		if( m_pUdpSocket->isValid())
			m_pUdpSocket->writeDatagram(&DummyData, 1, m_ServerIPAdr, m_ServerPort);
	}
}

//***********   C N e t i o   I m p l e m e n t a t i o n    ********
/////////////////////////////////////////////////////////////////////
// Constructor/Destructor
/////////////////////////////////////////////////////////////////////
CNetio::CNetio()
{
qDebug()<<"CNetio constructor";
	m_pTcpClient = new QTcpSocket;
	connect(m_pTcpClient, SIGNAL(readyRead()), this, SLOT(ReadTcpData()));
	connect(m_pTcpClient, SIGNAL(stateChanged(QAbstractSocket::SocketState)),
			this, SLOT(TcpStateChanged(QAbstractSocket::SocketState)));
	m_pUdpIo = new CUdp(this);
	emit NewStatus(NOT_CONNECTED);
}

CNetio::~CNetio()
{
qDebug()<<"CNetio destructor";
	if(m_pTcpClient->state() == QAbstractSocket::ConnectedState)
	{
		m_pTcpClient->abort();
		m_pTcpClient->close();
	}
	disconnect();
	if(m_pTcpClient)
		delete m_pTcpClient;
	if(m_pUdpIo)
		delete m_pUdpIo;
}

///////////////////////////////////////////////////////////////////////////////
// sends signal to indicate network/sdr status change
///////////////////////////////////////////////////////////////////////////////
void CNetio::SendStatus(eStatus status)
{
	m_Status = status;
//qDebug()<<"Status = "<<m_Status;
	emit NewStatus( status );
}

///////////////////////////////////////////////////////////////////////////////
// Called to connect to TCP Server SDR
///////////////////////////////////////////////////////////////////////////////
void CNetio::ConnectToServer(QHostAddress IPAdr, quint16 Port)
{
	m_MsgState = MSGSTATE_HDR1;
	m_RxMsgIndex = 0;
	m_RxMsgLength = 0;
	m_ServerIPAdr = IPAdr;
	m_ServerPort = Port;
	m_pTcpClient->abort();
	m_pTcpClient->close();
	emit NewStatus(CONNECTING);
	m_pTcpClient->connectToHost(m_ServerIPAdr, m_ServerPort);
qDebug()<<"Connecting to "<<m_ServerIPAdr <<m_ServerPort;
}

///////////////////////////////////////////////////////////////////////////////
// Called to disconnect from TCP Server SDR
///////////////////////////////////////////////////////////////////////////////
void CNetio::DisconnectFromServerSlot()
{
	m_pTcpClient->abort();
	m_pTcpClient->close();
	emit StopUdp();
	emit NewStatus(NOT_CONNECTED);
}

///////////////////////////////////////////////////////////////////////////////
// Called by tcp socket when status changes
///////////////////////////////////////////////////////////////////////////////
void CNetio::TcpStateChanged(QAbstractSocket::SocketState State)
{
//	qDebug()<<State;
	switch(State)
	{
		case QAbstractSocket::ConnectingState:
		case QAbstractSocket::HostLookupState:
			SendStatus(CONNECTING);
			break;
		case QAbstractSocket::ConnectedState:
			emit StartUdp(m_ServerIPAdr.toIPv4Address(), m_pTcpClient->localAddress().toIPv4Address(), m_ServerPort);
			SendStatus(CONNECTED);
			break;
		case QAbstractSocket::ClosingState:
			emit StopUdp();
			SendStatus(NOT_CONNECTED);
			break;
		default:
			break;
	}
}

///////////////////////////////////////////////////////////////////////////////
// Called by tcp socket when new TCP data is available
///////////////////////////////////////////////////////////////////////////////
void CNetio::ReadTcpData()
{
quint8 pBuf[10000];
	if(m_pTcpClient->isValid())
	{
		qint64 n = m_pTcpClient->bytesAvailable();
		if(n<10000)
		{
			m_pTcpClient->read((char*)pBuf, n);
			AssembleAscpMsg(pBuf, n);
		}
	}
}


/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/
/*                  -------------------------------                          */
/*                 | A s s e m b l e A s c p M s g |                         */
/*                  -------------------------------                          */
/*  Helper function to assemble TCP data stream into ASCP formatted messages */
/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/
void CNetio::AssembleAscpMsg(quint8* Buf, int Len)
{
	for(int i=0; i<Len; i++)
	{	//process everything in Buf
		switch(m_MsgState)	//Simple state machine to get generic ASCP msg
		{
			case MSGSTATE_HDR1:		//get first header byte of ASCP msg
				m_RxAscpMsg.Buf8[0] = Buf[i];
				m_MsgState = MSGSTATE_HDR2;	//go to get second header byte state
				break;
			case MSGSTATE_HDR2:	//here for second byte of header
				m_RxAscpMsg.Buf8[1] = Buf[i];
				m_RxMsgLength = m_RxAscpMsg.Buf16[0] & 0x1FFF;
				m_RxMsgIndex = 2;
				if(2 == m_RxMsgLength)	//if msg has no parameters then we are done
				{
					m_MsgState = MSGSTATE_HDR1;	//go back to first state
					ParseAscpMsg( &m_RxAscpMsg );
				}
				else	//there are data bytes to fetch
				{
					if( 0 == m_RxMsgLength)
						m_RxMsgLength = 8192+2;	//handle special case of 8192 byte data message
					m_MsgState = MSGSTATE_DATA;	//go to data byte reading state
				}
				break;
			case MSGSTATE_DATA:	//try to read the rest of the message
				m_RxAscpMsg.Buf8[m_RxMsgIndex++] = Buf[i];
				if( m_RxMsgIndex >= m_RxMsgLength )
				{
					m_MsgState = MSGSTATE_HDR1;	//go back to first stage
					m_RxMsgIndex = 0;
					ParseAscpMsg( &m_RxAscpMsg );	//got complete msg so call virtual parse..
				}
				break;
		} //end switch statement
	}
}

/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/
/*                     -----------------------                               */
/*                    | S e n d A s c p M s g |                              */
/*                     -----------------------                               */
/*   Called send an ASCP formated message to the TCP socket                  */
/*%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%*/
void CNetio::SendAscpMsg(CAscpTxMsg* pMsg)
{
	if(m_pTcpClient->isValid())
		m_pTcpClient->write((char*)pMsg->Buf8, (qint64)pMsg->GetLength());
}
