#!/usr/bin/env bash
# Brings up the whole stack on a local kind cluster and verifies it end to end.
# This is the exact sequence the README's results were produced with.
set -euo pipefail

CLUSTER="${CLUSTER:-matchbox}"
IMAGE="${IMAGE:-matchbox-engine:dev}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"

echo "==> creating kind cluster '${CLUSTER}' (skipped if it exists)"
kind get clusters | grep -qx "${CLUSTER}" || kind create cluster --name "${CLUSTER}"

echo "==> building the service image"
docker build -t "${IMAGE}" -f "${ROOT}/service/Dockerfile" "${ROOT}"

# kind nodes have their own image store, so a locally built image has to be side-loaded;
# there is no registry in this setup.
echo "==> loading the image into the cluster"
kind load docker-image "${IMAGE}" --name "${CLUSTER}"

echo "==> deploying kafka"
kubectl apply -f "${HERE}/kafka.yaml"
kubectl rollout status statefulset/kafka --timeout=600s

echo "==> creating topics"
kubectl apply -f "${HERE}/kafka-topics-job.yaml"
kubectl wait --for=condition=complete job/kafka-topics --timeout=300s

echo "==> deploying the engine StatefulSet"
kubectl apply -f "${HERE}/engine.yaml"
kubectl rollout status statefulset/matchbox-engine --timeout=600s

echo "==> partition ownership as claimed by each pod"
for i in 0 1 2 3; do
  kubectl logs "matchbox-engine-${i}" | grep -E 'partitions=' || true
done

# Same observability stack as the EKS deployment, so the dashboard can be produced and
# recorded locally without paying for a cloud cluster.
if [[ "${WITH_MONITORING:-1}" == "1" ]]; then
  echo "==> installing kube-prometheus-stack (set WITH_MONITORING=0 to skip)"
  helm repo add prometheus-community https://prometheus-community.github.io/helm-charts >/dev/null 2>&1 || true
  helm repo update >/dev/null
  helm upgrade --install monitoring prometheus-community/kube-prometheus-stack \
    --namespace monitoring --create-namespace \
    --set alertmanager.enabled=false \
    --set prometheus.prometheusSpec.retention=6h \
    --set grafana.sidecar.dashboards.searchNamespace=ALL \
    --set prometheus.prometheusSpec.podMonitorSelectorNilUsesHelmValues=false \
    --set prometheus.prometheusSpec.serviceMonitorSelectorNilUsesHelmValues=false \
    --wait --timeout 15m

  kubectl apply -f "${HERE}/engine-servicemonitor.yaml"
  kubectl apply -f "${HERE}/grafana-dashboard.yaml"
fi

echo "==> running end-to-end verification"
kubectl delete pod matchbox-e2e --ignore-not-found >/dev/null
kubectl run matchbox-e2e \
  --image="${IMAGE}" --image-pull-policy=IfNotPresent --restart=Never \
  --env="KAFKA_BROKERS=kafka-0.kafka.default.svc.cluster.local:9092" \
  --env="STEPS=${STEPS:-5000}" --env="WAIT_SECONDS=120" \
  --command -- /usr/local/bin/matchbox_e2e >/dev/null

kubectl wait --for=jsonpath='{.status.phase}'=Succeeded pod/matchbox-e2e --timeout=600s \
  || kubectl wait --for=jsonpath='{.status.phase}'=Failed pod/matchbox-e2e --timeout=10s
kubectl logs matchbox-e2e

exit_code="$(kubectl get pod matchbox-e2e -o jsonpath='{.status.containerStatuses[0].state.terminated.exitCode}')"
echo "==> verification exit code: ${exit_code}"

if [[ "${WITH_MONITORING:-1}" == "1" ]]; then
  cat <<'EOF'

==> to record the dashboard:
    1. generate traffic (leave running in another terminal):
         kubectl run matchbox-load --image=matchbox-engine:dev --restart=Never \
           --env=KAFKA_BROKERS=kafka-0.kafka.default.svc.cluster.local:9092 \
           --env=LOAD_ONLY=1 --env=LOAD_SECONDS=300 --env=STEPS=5000 \
           --command -- /usr/local/bin/matchbox_e2e
    2. read the generated Grafana password (created by the chart, never committed):
         kubectl -n monitoring get secret monitoring-grafana \
           -o jsonpath="{.data.admin-password}" | base64 -d; echo
    3. open Grafana as user "admin":
         kubectl -n monitoring port-forward svc/monitoring-grafana 3000:80
         http://localhost:3000  ->  dashboard "matchbox engine"
EOF
fi

echo "==> tear down with: kind delete cluster --name ${CLUSTER}"
exit "${exit_code}"
