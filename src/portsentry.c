/************************************************************************/
/*                                                                      */
/* PortSentry                                                           */
/*                                                                      */
/* This software is Copyright(c) 1997-2003 Craig Rowland                */
/*                                                                      */
/* This software is covered under the Common Public License v1.0        */
/* See the enclosed LICENSE file for more information.                  */
/*                                                                      */
/* Created: 10-12-1997                                                  */
/* Modified: 05-23-2003                                                 */
/*                                                                      */
/* Send all changes/modifications/bugfixes to:                          */
/* craigrowland at users dot sourceforge dot net                        */
/*                                                                      */
/* $Id: portsentry.c,v 1.40 2003/05/23 17:41:25 crowland Exp crowland $ */
/************************************************************************/

#include "portsentry.h"
#include "cmdline.h"
#include "config_data.h"
#include "configfile.h"
#include "connect_sentry.h"
#include "portsentry_io.h"
#include "portsentry_util.h"
#include "state_machine.h"

static int IsPortInUse(uint16_t port, int proto);

#ifdef SUPPORT_STEALTH
static int PortSentryStealthModeTCP(void);
static int PortSentryAdvancedStealthModeTCP(void);
static int PortSentryStealthModeUDP(void);
static int PortSentryAdvancedStealthModeUDP(void);
static int PacketRead(int socket, char *packetBuffer, size_t packetBufferSize, struct iphdr **ipPtr, void **transportPtr);
static char *ReportPacketType(struct tcphdr *);
#endif

static int EvalPortsInUse(int *portCount, int *ports);

int main(int argc, char *argv[]) {
  ParseCmdline(argc, argv);

  readConfigFile();

  if (configData.logFlags & LOGFLAG_DEBUG) {
    printf("Final Configuration:\n");
    PrintConfigData(configData);
  }

  if ((geteuid()) && (getuid()) != 0) {
    printf("You need to be root to run this.\n");
    Exit(ERROR);
  }

  if (configData.daemon == TRUE) {
    if (DaemonSeed() == ERROR) {
      Log("adminalert: ERROR: could not go into daemon mode. Shutting down.");
      printf("ERROR: could not go into daemon mode. Shutting down.\n");
      Exit(ERROR);
    }
  }

  if (configData.sentryMode == SENTRY_MODE_TCP || configData.sentryMode == SENTRY_MODE_UDP) {
    if (PortSentryConnectMode() == ERROR) {
      Log("adminalert: ERROR: could not go into PortSentry mode. Shutting down.");
      Exit(ERROR);
    }
  }
#ifdef SUPPORT_STEALTH
  else if (configData.sentryMode == SENTRY_MODE_STCP) {
    if (PortSentryStealthModeTCP() == ERROR) {
      Log("adminalert: ERROR: could not go into PortSentry mode. Shutting down.");
      Exit(ERROR);
    }
  } else if (configData.sentryMode == SENTRY_MODE_ATCP) {
    if (PortSentryAdvancedStealthModeTCP() == ERROR) {
      Log("adminalert: ERROR: could not go into PortSentry mode. Shutting down.");
      Exit(ERROR);
    }
  } else if (configData.sentryMode == SENTRY_MODE_SUDP) {
    if (PortSentryStealthModeUDP() == ERROR) {
      Log("adminalert: ERROR: could not go into PortSentry mode. Shutting down.");
      Exit(ERROR);
    }
  } else if (configData.sentryMode == SENTRY_MODE_AUDP) {
    if (PortSentryAdvancedStealthModeUDP() == ERROR) {
      Log("adminalert: ERROR: could not go into PortSentry mode. Shutting down.");
      Exit(ERROR);
    }
  }
#endif

  return EXIT_SUCCESS;
}

#ifdef SUPPORT_STEALTH

/* Read packet IP and transport headers and set ipPtr/transportPtr to their correct location
 * transportPtr is either a struct tcphdr * or struct udphdr *
 */
static int PacketRead(int socket, char *packetBuffer, size_t packetBufferSize, struct iphdr **ipPtr, void **transportPtr) {
  size_t ipHeaderLength;
  struct in_addr addr;

  if (read(socket, packetBuffer, packetBufferSize) == -1) {
    Log("adminalert: ERROR: Could not read from socket %d: (errno: %d). Aborting", socket, errno);
    return ERROR;
  }

  *ipPtr = (struct iphdr *)packetBuffer;

  if (((*ipPtr)->ihl < 5) || ((*ipPtr)->ihl > 15)) {
    addr.s_addr = (u_int)(*ipPtr)->saddr;
    Log("attackalert: Illegal IP header length detected in TCP packet: %d from (possible) host: %s", (*ipPtr)->ihl, inet_ntoa(addr));
    return (FALSE);
  }

  ipHeaderLength = (*ipPtr)->ihl * 4;

  if (ipHeaderLength > packetBufferSize) {
    Log("adminalert: ERROR: IP header length (%lu) is larger than packet buffer size (%lu). Aborting", ipHeaderLength, packetBufferSize);
    return FALSE;
  }

  *transportPtr = (void *)(packetBuffer + ipHeaderLength);
  return TRUE;
}

static int EvalPortsInUse(int *portCount, int *ports) {
  int portsLength, i, gotBound = FALSE, status;
  uint16_t *portList;
  int proto;

  *portCount = 0;

  if (configData.sentryMode == SENTRY_MODE_STCP || configData.sentryMode == SENTRY_MODE_ATCP) {
    portsLength = configData.tcpPortsLength;
    portList = configData.tcpPorts;
    proto = IPPROTO_TCP;
  } else if (configData.sentryMode == SENTRY_MODE_SUDP || configData.sentryMode == SENTRY_MODE_AUDP) {
    portsLength = configData.udpPortsLength;
    portList = configData.udpPorts;
    proto = IPPROTO_UDP;
  } else {
    Log("Invalid sentry mode in EvalPortsInUse");
    return (FALSE);
  }

  for (i = 0; i < portsLength; i++) {
    Log("Going into stealth listen mode on port: %d", portList[i]);
    status = IsPortInUse(portList[i], proto);

    if (status == FALSE) {
      gotBound = TRUE;
      ports[(*portCount)++] = portList[i];
    } else if (status == TRUE) {
      Log("Socket %d is in use and will not be monitored. Attempting to continue", portList[i]);
    } else if (status == ERROR) {
      return FALSE;
    }
  }

  if (gotBound == FALSE) {
    Log("No ports were bound. Aborting");
  }

  return gotBound;
}

/****************************************************************/
/* Stealth scan detection Mode One                              */
/*                                                              */
/* This mode will read in a list of ports to monitor and will   */
/* then open a raw socket to look for packets matching the port. */
/*                                                              */
/****************************************************************/
static int PortSentryStealthModeTCP(void) {
  int portCount2, ports2[MAXSOCKS];
  int count = 0, scanDetectTrigger = TRUE, result = TRUE;
  int openSockfd = 0, incomingPort = 0;
  char target[IPMAXBUF];
  char resolvedHost[DNSMAXBUF], *packetType;
  char packetBuffer[TCPPACKETLEN];
  struct in_addr addr;
  struct iphdr *ip;
  struct tcphdr *tcp;

  /* ok, now check if they have a network daemon on the socket already, if they
   * do then skip that port because it will cause false alarms */
  if (EvalPortsInUse(&portCount2, ports2) == FALSE) {
    return ERROR; // Error msg in function
  }

  /* Open our raw socket for network IO */
  if ((openSockfd = OpenRAWTCPSocket()) == ERROR) {
    Log("adminalert: ERROR: could not open RAW TCP socket. Aborting.");
    return (ERROR);
  }

  Log("adminalert: PortSentry is now active and listening.");

  /* main detection loop */
  for (;;) {
    if (PacketRead(openSockfd, packetBuffer, TCPPACKETLEN, &ip, (void **)&tcp) != TRUE)
      continue;

    incomingPort = ntohs(tcp->dest);

    /* check for an ACK/RST to weed out established connections in case the user is monitoring high ephemeral port numbers */
    if ((tcp->ack != 1) && (tcp->rst != 1)) {
      /* this iterates the list of ports looking for a match */
      for (count = 0; count < portCount2; count++) {
        if (incomingPort == ports2[count]) {
          if (IsPortInUse(incomingPort, IPPROTO_TCP) == TRUE)
            break;

          /* copy the clients address into our buffer for nuking */
          addr.s_addr = (u_int)ip->saddr;
          SafeStrncpy(target, (char *)inet_ntoa(addr), IPMAXBUF);
          /* check if we should ignore this IP */
          result = NeverBlock(target, configData.ignoreFile);

          if (result == ERROR) {
            Log("attackalert: ERROR: cannot open ignore file. Blocking host anyway.");
            result = FALSE;
          }

          if (result == FALSE) {
            /* check if they've visited before */
            scanDetectTrigger = CheckStateEngine(target);
            if (scanDetectTrigger == TRUE) {
              if (configData.resolveHost) { /* Do they want DNS resolution? */
                if (CleanAndResolve(resolvedHost, target) != TRUE) {
                  Log("attackalert: ERROR: Error resolving host. resolving disabled for this host.");
                  snprintf(resolvedHost, DNSMAXBUF, "%s", target);
                }
              } else {
                snprintf(resolvedHost, DNSMAXBUF, "%s", target);
              }

              packetType = ReportPacketType(tcp);
              Log("attackalert: %s from host: %s/%s to TCP port: %d", packetType, resolvedHost, target, ports2[count]);
              /* Report on options present */
              if (ip->ihl > 5)
                Log("attackalert: Packet from host: %s/%s to TCP port: %d has IP options set (detection avoidance technique).",
                    resolvedHost, target, ports2[count]);

              /* check if this target is already blocked */
              if (IsBlocked(target, configData.blockedFile) == FALSE) {
                /* toast the prick */
                if (DisposeTarget(target, ports2[count], IPPROTO_TCP) != TRUE)
                  Log("attackalert: ERROR: Could not block host %s/%s !!", resolvedHost, target);
                else
                  WriteBlocked(target, resolvedHost, ports2[count], configData.blockedFile, configData.historyFile, "TCP");
              } else { /* end IsBlocked check */
                Log("attackalert: Host: %s/%s is already blocked Ignoring", resolvedHost, target);
              }
            }    /* end if(scanDetectTrigger) */
          }      /* end if(never block) check */
          break; /* get out of for(count) loop above */
        }        /* end if(incoming port) ==  protected port */
      }          /* end for( check for protected port loop ) loop */
    }            /* end if(TH_ACK) check */
  }              /* end for( ; ; ) loop */
} /* end PortSentryStealthModeTCP */

/****************************************************************/
/* Advanced Stealth scan detection Mode One                     */
/*                                                              */
/* This mode will see what ports are listening below 1024       */
/* and will then monitor all the rest. This is very sensitive   */
/* and will react on any packet hitting any monitored port,     */
/* regardless of TCP flags set                                  */
/*                                                              */
/****************************************************************/
static int PortSentryAdvancedStealthModeTCP(void) {
  int result = TRUE, scanDetectTrigger = TRUE, hotPort = TRUE;
  int openSockfd = 0, smartVerify = FALSE, i;
  unsigned int incomingPort = 0;
  unsigned int count = 0, inUsePorts[MAXSOCKS], portCount = 0;
  char target[IPMAXBUF];
  char resolvedHost[DNSMAXBUF], *packetType;
  char packetBuffer[TCPPACKETLEN];
  struct in_addr addr;
  struct iphdr *ip;
  struct tcphdr *tcp;

  Log("adminalert: Advanced mode will monitor first %d ports", configData.tcpAdvancedPort);

  /* try to bind to all ports below 1024, any that are taken we exclude later */
  for (count = 0; count < configData.tcpAdvancedPort; count++) {
    if ((openSockfd = OpenTCPSocket()) == ERROR) {
      Log("adminalert: ERROR: could not open TCP socket. Aborting.");
      return (ERROR);
    }
    if (BindSocket(openSockfd, count) == ERROR)
      inUsePorts[portCount++] = count;

    close(openSockfd);
  }

  // FIXME: Don't add duplicate ports in inUsePorts
  if (configData.tcpAdvancedExcludePortsLength > 0) {
    for (i = 0; i < configData.tcpAdvancedExcludePortsLength; i++) {
      inUsePorts[portCount++] = configData.tcpAdvancedExcludePorts[count];
      Log("Advanced mode will manually exclude port: %d ", inUsePorts[portCount - 1]);
    }
  } else {
    Log("Advanced mode will manually exclude no ports");
  }

  for (count = 0; count < portCount; count++)
    Log("adminalert: Advanced Stealth scan detection mode activated. Ignored TCP port: %d", inUsePorts[count]);

  /* open raw socket for reading */
  if ((openSockfd = OpenRAWTCPSocket()) == ERROR) {
    Log("adminalert: ERROR: could not open RAW TCP socket. Aborting.");
    return (ERROR);
  }

  Log("adminalert: PortSentry is now active and listening.");

  /* main detection loop */
  for (;;) {
    if (PacketRead(openSockfd, packetBuffer, TCPPACKETLEN, &ip, (void **)&tcp) != TRUE)
      continue;

    incomingPort = ntohs(tcp->dest);

    /* don't monitor packets with ACK set (established) or RST */
    /* This could be a hole in some cases */
    if ((tcp->ack != 1) && (tcp->rst != 1)) {
      /* check if we should ignore this connection to this port */
      for (count = 0; count < portCount; count++) {
        if ((incomingPort == inUsePorts[count]) || (incomingPort >= configData.tcpAdvancedPort)) {
          hotPort = FALSE;
          break;
        } else {
          hotPort = TRUE;
        }
      }

      if (hotPort) {
        smartVerify = IsPortInUse(incomingPort, IPPROTO_TCP);

        // FIXME: IsPortInUse returns true, false, error
        if (smartVerify != TRUE) {
          addr.s_addr = (u_int)ip->saddr;
          SafeStrncpy(target, (char *)inet_ntoa(addr), IPMAXBUF);
          /* check if we should ignore this IP */
          result = NeverBlock(target, configData.ignoreFile);

          if (result == ERROR) {
            Log("attackalert: ERROR: cannot open ignore file. Blocking host anyway.");
            result = FALSE;
          }

          if (result == FALSE) {
            /* check if they've visited before */
            scanDetectTrigger = CheckStateEngine(target);

            if (scanDetectTrigger == TRUE) {
              if (configData.resolveHost) { /* Do they want DNS resolution? */
                if (CleanAndResolve(resolvedHost, target) != TRUE) {
                  Log("attackalert: ERROR: Error resolving host. resolving disabled for this host.");
                  snprintf(resolvedHost, DNSMAXBUF, "%s", target);
                }
              } else {
                snprintf(resolvedHost, DNSMAXBUF, "%s", target);
              }

              packetType = ReportPacketType(tcp);
              Log("attackalert: %s from host: %s/%s to TCP port: %u", packetType, resolvedHost, target, incomingPort);
              /* Report on options present */
              if (ip->ihl > 5)
                Log("attackalert: Packet from host: %s/%s to TCP port: %u has IP options set (detection avoidance technique).",
                    resolvedHost, target, incomingPort);

              /* check if this target is already blocked */
              if (IsBlocked(target, configData.blockedFile) == FALSE) {
                /* toast the prick */
                if (DisposeTarget(target, incomingPort, IPPROTO_TCP) != TRUE)
                  Log("attackalert: ERROR: Could not block host %s/%s!!", resolvedHost, target);
                else
                  WriteBlocked(target, resolvedHost, incomingPort, configData.blockedFile, configData.historyFile, "TCP");
              } else { /* end IsBlocked check */
                Log("attackalert: Host: %s/%s is already blocked Ignoring", resolvedHost, target);
              }
            } /* end if(scanDetectTrigger) */
          }   /* end if(never block) check */
        }     /* end if(smartVerify) */
      }       /* end if(hotPort) */
    }         /* end if(TH_ACK) */
  }           /* end for( ; ; ) loop */
}
/* end PortSentryAdvancedStealthModeTCP */

/****************************************************************/
/* UDP "stealth" scan detection                                 */
/*                                                              */
/* This mode will read in a list of ports to monitor and will   */
/* then open a raw socket to look for packets matching the port. */
/*                                                              */
/****************************************************************/
static int PortSentryStealthModeUDP(void) {
  int portCount2 = 0, ports2[MAXSOCKS], result = TRUE;
  int count = 0, scanDetectTrigger = TRUE;
  int openSockfd = 0, incomingPort = 0;
  char target[IPMAXBUF];
  char resolvedHost[DNSMAXBUF];
  char packetBuffer[UDPPACKETLEN];
  struct in_addr addr;
  struct iphdr *ip;
  struct udphdr *udp;

  /* ok, now check if they have a network daemon on the socket already, if they
   * do then skip that port because it will cause false alarms */
  if (EvalPortsInUse(&portCount2, ports2) == FALSE) {
    return ERROR; // Error msg in function
  }

  if ((openSockfd = OpenRAWUDPSocket()) == ERROR) {
    Log("adminalert: ERROR: could not open RAW UDP socket. Aborting.");
    return (ERROR);
  }

  Log("adminalert: PortSentry is now active and listening.");

  /* main detection loop */
  for (;;) {
    if (PacketRead(openSockfd, packetBuffer, UDPPACKETLEN, &ip, (void **)&udp) != TRUE)
      continue;

    incomingPort = ntohs(udp->dest);

    /* this iterates the list of ports looking for a match */
    for (count = 0; count < portCount2; count++) {
      if (incomingPort == ports2[count]) {
        if (IsPortInUse(incomingPort, IPPROTO_UDP) == TRUE)
          break;

        addr.s_addr = (u_int)ip->saddr;
        SafeStrncpy(target, (char *)inet_ntoa(addr), IPMAXBUF);
        /* check if we should ignore this IP */
        result = NeverBlock(target, configData.ignoreFile);

        if (result == ERROR) {
          Log("attackalert: ERROR: cannot open ignore file. Blocking host anyway.");
          result = FALSE;
        }

        if (result == FALSE) {
          /* check if they've visited before */
          scanDetectTrigger = CheckStateEngine(target);
          if (scanDetectTrigger == TRUE) {
            if (configData.resolveHost) { /* Do they want DNS resolution? */
              if (CleanAndResolve(resolvedHost, target) != TRUE) {
                Log("attackalert: ERROR: Error resolving host. resolving disabled for this host.");
                snprintf(resolvedHost, DNSMAXBUF, "%s", target);
              }
            } else {
              snprintf(resolvedHost, DNSMAXBUF, "%s", target);
            }

            Log("attackalert: UDP scan from host: %s/%s to UDP port: %d", resolvedHost, target, ports2[count]);
            /* Report on options present */
            if (ip->ihl > 5)
              Log("attackalert: Packet from host: %s/%s to UDP port: %d has IP options set (detection avoidance technique).",
                  resolvedHost, target, incomingPort);

            /* check if this target is already blocked */
            if (IsBlocked(target, configData.blockedFile) == FALSE) {
              if (DisposeTarget(target, ports2[count], IPPROTO_UDP) != TRUE)
                Log("attackalert: ERROR: Could not block host %s/%s!!", resolvedHost, target);
              else
                WriteBlocked(target, resolvedHost, ports2[count], configData.blockedFile, configData.historyFile, "UDP");
            } else { /* end IsBlocked check */
              Log("attackalert: Host: %s/%s is already blocked Ignoring", resolvedHost, target);
            }
          }    /* end if(scanDetectTrigger) */
        }      /* end if(never block) check */
        break; /* get out of for(count) loop above */
      }        /* end if(incoming port) ==  protected port */
    }          /* end for( check for protected port loop ) loop */
  }            /* end for( ; ; ) loop */
} /* end PortSentryStealthModeUDP */

/****************************************************************/
/* Advanced Stealth scan detection mode for UDP                 */
/*                                                              */
/* This mode will see what ports are listening below 1024       */
/* and will then monitor all the rest. This is very sensitive   */
/* and will react on any packet hitting any monitored port.     */
/* This is a very dangerous option and is for advanced users    */
/*                                                              */
/****************************************************************/
static int PortSentryAdvancedStealthModeUDP(void) {
  int result = TRUE, scanDetectTrigger = TRUE, hotPort = TRUE;
  int openSockfd = 0, smartVerify = FALSE, i;
  unsigned int incomingPort = 0;
  unsigned int count = 0, inUsePorts[MAXSOCKS], portCount = 0;
  char target[IPMAXBUF];
  char resolvedHost[DNSMAXBUF];
  char packetBuffer[UDPPACKETLEN];
  struct in_addr addr;
  struct iphdr *ip;
  struct udphdr *udp;

  Log("adminalert: Advanced mode will monitor first %d ports", configData.udpAdvancedPort);

  /* try to bind to all ports below 1024, any that are taken we exclude later */
  for (count = 0; count < configData.udpAdvancedPort; count++) {
    if ((openSockfd = OpenUDPSocket()) == ERROR) {
      Log("adminalert: ERROR: could not open UDP socket. Aborting.");
      return (ERROR);
    }
    if (BindSocket(openSockfd, count) == ERROR)
      inUsePorts[portCount++] = count;

    close(openSockfd);
  }

  // FIXME: Don't add duplicate ports in inUsePorts
  if (configData.udpAdvancedExcludePortsLength > 0) {
    for (i = 0; i < configData.udpAdvancedExcludePortsLength; i++) {
      inUsePorts[portCount++] = configData.udpAdvancedExcludePorts[count];
      Log("Advanced mode will manually exclude port: %d ", inUsePorts[portCount - 1]);
    }
  } else {
    Log("Advanced mode will manually exclude no ports");
  }

  for (count = 0; count < portCount; count++) {
    Log("adminalert: Advanced Stealth scan detection mode activated. Ignored UDP port: %d", inUsePorts[count]);
  }

  if ((openSockfd = OpenRAWUDPSocket()) == ERROR) {
    Log("adminalert: ERROR: could not open RAW UDP socket. Aborting.");
    return (ERROR);
  }

  Log("adminalert: PortSentry is now active and listening.");

  /* main detection loop */
  for (;;) {
    if (PacketRead(openSockfd, packetBuffer, UDPPACKETLEN, &ip, (void **)&udp) != TRUE)
      continue;

    incomingPort = ntohs(udp->dest);

    /* check if we should ignore this connection to this port */
    for (count = 0; count < portCount; count++) {
      if ((incomingPort == inUsePorts[count]) || (incomingPort >= configData.udpAdvancedPort)) {
        hotPort = FALSE;
        break;
      } else {
        hotPort = TRUE;
      }
    }

    if (hotPort) {
      smartVerify = IsPortInUse(incomingPort, IPPROTO_UDP);

      // FIXME: IsPortInUse returns true, false, error
      if (smartVerify != TRUE) {
        /* copy the clients address into our buffer for nuking */
        addr.s_addr = (u_int)ip->saddr;
        SafeStrncpy(target, (char *)inet_ntoa(addr), IPMAXBUF);
        /* check if we should ignore this IP */
        result = NeverBlock(target, configData.ignoreFile);

        if (result == ERROR) {
          Log("attackalert: ERROR: cannot open ignore file. Blocking host anyway.");
          result = FALSE;
        }

        if (result == FALSE) {
          /* check if they've visited before */
          scanDetectTrigger = CheckStateEngine(target);

          if (scanDetectTrigger == TRUE) {
            if (configData.resolveHost) { /* Do they want DNS resolution? */
              if (CleanAndResolve(resolvedHost, target) != TRUE) {
                Log("attackalert: ERROR: Error resolving host. resolving disabled for this host.");
                snprintf(resolvedHost, DNSMAXBUF, "%s", target);
              }
            } else {
              snprintf(resolvedHost, DNSMAXBUF, "%s", target);
            }

            Log("attackalert: UDP scan from host: %s/%s to UDP port: %u", resolvedHost, target, incomingPort);
            /* Report on options present */
            if (ip->ihl > 5)
              Log("attackalert: Packet from host: %s/%s to UDP port: %u has IP options set (detection avoidance technique).", resolvedHost, target, incomingPort);

            /* check if this target is already blocked */
            if (IsBlocked(target, configData.blockedFile) == FALSE) {
              if (DisposeTarget(target, incomingPort, IPPROTO_UDP) != TRUE)
                Log("attackalert: ERROR: Could not block host %s/%s!!", resolvedHost, target);
              else
                WriteBlocked(target, resolvedHost, incomingPort, configData.blockedFile, configData.historyFile, "UDP");
            } else { /* end IsBlocked check */
              Log("attackalert: Host: %s/%s is already blocked Ignoring", resolvedHost, target);
            }
          } /* end if(scanDetectTrigger) */
        }   /* end if(never block) check */
      }     /* end if (smartVerify) */
    }       /* end if(hotPort) */
  }         /* end for( ; ; ) loop */
}
/* end PortSentryAdvancedStealthModeUDP */

#endif

#ifdef SUPPORT_STEALTH
/* This takes a tcp packet and reports what type of scan it is */
static char *ReportPacketType(struct tcphdr *tcpPkt) {
  static char packetDesc[MAXBUF];
  static char *packetDescPtr = packetDesc;

  if ((tcpPkt->syn == 0) && (tcpPkt->fin == 0) && (tcpPkt->ack == 0) &&
      (tcpPkt->psh == 0) && (tcpPkt->rst == 0) && (tcpPkt->urg == 0))
    snprintf(packetDesc, MAXBUF, " TCP NULL scan");
  else if ((tcpPkt->fin == 1) && (tcpPkt->urg == 1) && (tcpPkt->psh == 1))
    snprintf(packetDesc, MAXBUF, "TCP XMAS scan");
  else if ((tcpPkt->fin == 1) && (tcpPkt->syn != 1) && (tcpPkt->ack != 1) &&
           (tcpPkt->psh != 1) && (tcpPkt->rst != 1) && (tcpPkt->urg != 1))
    snprintf(packetDesc, MAXBUF, "TCP FIN scan");
  else if ((tcpPkt->syn == 1) && (tcpPkt->fin != 1) && (tcpPkt->ack != 1) &&
           (tcpPkt->psh != 1) && (tcpPkt->rst != 1) && (tcpPkt->urg != 1))
    snprintf(packetDesc, MAXBUF, "TCP SYN/Normal scan");
  else
    snprintf(packetDesc, MAXBUF,
             "Unknown Type: TCP Packet Flags: SYN: %d FIN: %d ACK: %d PSH: %d URG: %d RST: %d",
             tcpPkt->syn, tcpPkt->fin, tcpPkt->ack, tcpPkt->psh, tcpPkt->urg, tcpPkt->rst);

  return (packetDescPtr);
}

static int IsPortInUse(uint16_t port, int proto) {
  int testSockfd;

  assert(proto == IPPROTO_TCP || proto == IPPROTO_UDP);

  if (proto == IPPROTO_TCP) {
    testSockfd = OpenTCPSocket();
  } else if (proto == IPPROTO_UDP) {
    testSockfd = OpenUDPSocket();
  } else {
    Log("adminalert: ERROR: invalid protocol type passed to IsPortInUse.");
    return (ERROR);
  }

  if (testSockfd == ERROR) {
    Log("adminalert: ERROR: could not open %s socket to smart-verify.", proto == IPPROTO_TCP ? "TCP" : "UDP");
    return (ERROR);
  }

  if (BindSocket(testSockfd, port) == ERROR) {
    Debug("IsPortInUse: %d = Yes", port);
    close(testSockfd);
    return (TRUE);
  }

  close(testSockfd);
  return (FALSE);
}
#endif /* SUPPORT_STEALTH */
