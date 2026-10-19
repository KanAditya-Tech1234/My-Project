import React, { useEffect, useRef, useState } from 'react';
import { ActivityIndicator, Image, RefreshControl, ScrollView, StyleSheet, Text, View } from 'react-native';
import { StatusBar } from 'expo-status-bar';

// The Render service that both trackers post to. No trailing slash.
const SERVER_BASE = 'https://tracker-drone.onrender.com';

// The app checks this small status endpoint every second and only redraws
// when something new has arrived, so data shows up at whatever delay is
// selected on the website.
const POLL_MS = 1000;

function secondsAgo(iso, skewMs) {
  if (!iso) return null;
  return Math.max(0, (Date.now() + skewMs - new Date(iso).getTime()) / 1000);
}

export default function App() {
  const [status, setStatus] = useState(null);
  const [loading, setLoading] = useState(true);
  const [refreshing, setRefreshing] = useState(false);
  const [error, setError] = useState(false);
  const [, setNow] = useState(Date.now());
  const skewRef = useRef(0);

  async function fetchStatus() {
    try {
      const response = await fetch(`${SERVER_BASE}/api/status`, { cache: 'no-store' });
      const json = await response.json();
      skewRef.current = new Date(json.serverTime).getTime() - Date.now();
      setStatus(prev => (prev && prev.seq === json.seq && prev.intervalMs === json.intervalMs ? prev : json));
      setError(false);
    } catch (e) {
      console.log('Fetch error:', e);
      setError(true);
    } finally {
      setLoading(false);
      setRefreshing(false);
    }
  }

  useEffect(() => {
    fetchStatus();
    const poll = setInterval(fetchStatus, POLL_MS);
    const clock = setInterval(() => setNow(Date.now()), 500);   // keeps "seconds ago" ticking
    return () => { clearInterval(poll); clearInterval(clock); };
  }, []);

  const onRefresh = () => {
    setRefreshing(true);
    fetchStatus();
  };

  const data = status && status.latest;
  const photo = status && status.latestPhoto;
  const intervalS = status ? status.intervalMs / 1000 : null;
  const lastIso = [data && data.receivedAt, photo && photo.receivedAt].filter(Boolean).sort().pop();
  const ago = secondsAgo(lastIso, skewRef.current);
  const live = ago != null && status && ago * 1000 <= status.intervalMs + 5000;

  return (
    <View style={styles.container}>
      <StatusBar style="light" />

      <Text style={styles.title}>Drone Tracker</Text>
      <Text style={styles.subtitle}>
        {intervalS != null ? `Updates every ${intervalS} s` : 'Android live status'}
      </Text>

      {loading && !refreshing ? (
        <View style={styles.loadingBox}>
          <ActivityIndicator size="large" color="#60a5fa" />
          <Text style={styles.loadingText}>Loading live drone data...</Text>
          <Text style={styles.helperText}>The free server can take up to a minute to wake up.</Text>
        </View>
      ) : (
        <ScrollView
          contentContainerStyle={styles.cardList}
          refreshControl={<RefreshControl refreshing={refreshing} onRefresh={onRefresh} tintColor="#60a5fa" />}
        >
          {error && !status ? (
            <View style={styles.card}>
              <Text style={styles.label}>Connection Status</Text>
              <Text style={styles.errorText}>Unable to connect to the server.</Text>
              <Text style={styles.helperText}>Check SERVER_BASE in App.js and your internet connection.</Text>
            </View>
          ) : (
            <>
              <View style={styles.card}>
                <Text style={styles.label}>Status</Text>
                <Text style={[styles.value, { color: live ? '#4ade80' : '#fb923c' }]}>
                  {ago == null ? 'Waiting for drone...' : live ? 'Live' : 'No recent data'}
                </Text>
                {ago != null && <Text style={styles.helperText}>Last data {ago.toFixed(0)} s ago</Text>}
              </View>

              {photo && (
                <View style={styles.card}>
                  <Text style={styles.label}>Latest Photo</Text>
                  <Image
                    source={{ uri: `${SERVER_BASE}${photo.url}` }}
                    style={styles.photo}
                    resizeMode="contain"
                  />
                  <Text style={styles.helperText}>
                    {photo.trackerId} · {photo.timestamp || photo.receivedAt} · {Math.round(photo.size / 1024)} KB
                  </Text>
                </View>
              )}

              {data ? (
                <>
                  <View style={styles.card}>
                    <Text style={styles.label}>Tracker ID</Text>
                    <Text style={styles.value}>{data.trackerId || 'N/A'}</Text>
                  </View>

                  <View style={styles.card}>
                    <Text style={styles.label}>Latitude / Longitude</Text>
                    <Text style={styles.value}>{data.lat?.toFixed(6) ?? 'N/A'}, {data.lon?.toFixed(6) ?? 'N/A'}</Text>
                  </View>

                  <View style={styles.card}>
                    <Text style={styles.label}>Altitude</Text>
                    <Text style={styles.value}>{data.altitude || 'N/A'}</Text>
                  </View>

                  <View style={styles.card}>
                    <Text style={styles.label}>Last Update</Text>
                    <Text style={styles.value}>{data.timestamp || data.receivedAt || 'N/A'}</Text>
                  </View>
                </>
              ) : (
                <View style={styles.card}>
                  <Text style={styles.label}>Position</Text>
                  <Text style={styles.helperText}>No GPS position received yet.</Text>
                </View>
              )}
            </>
          )}
        </ScrollView>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: '#0f172a',
    paddingTop: 70,
    paddingHorizontal: 20,
  },
  title: {
    color: '#f8fafc',
    fontSize: 32,
    fontWeight: '700',
  },
  subtitle: {
    color: '#93c5fd',
    fontSize: 16,
    marginBottom: 20,
  },
  loadingBox: {
    flex: 1,
    justifyContent: 'center',
    alignItems: 'center',
  },
  loadingText: {
    color: '#e2e8f0',
    marginTop: 12,
    fontSize: 16,
  },
  cardList: {
    paddingBottom: 30,
  },
  card: {
    backgroundColor: '#111827',
    borderRadius: 14,
    padding: 18,
    marginBottom: 14,
    borderWidth: 1,
    borderColor: '#1f2937',
  },
  label: {
    color: '#93c5fd',
    fontSize: 12,
    textTransform: 'uppercase',
    letterSpacing: 1,
    marginBottom: 8,
  },
  value: {
    color: '#f8fafc',
    fontSize: 20,
    fontWeight: '600',
  },
  photo: {
    width: '100%',
    aspectRatio: 4 / 3,
    borderRadius: 8,
    backgroundColor: '#000',
  },
  errorText: {
    color: '#fca5a5',
    fontSize: 18,
    fontWeight: '600',
  },
  helperText: {
    color: '#cbd5e1',
    marginTop: 8,
    fontSize: 14,
  },
});
